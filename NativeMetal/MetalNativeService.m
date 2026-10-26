#import "PrivateMetalProbe.h"
#include "Backend.h"
#include "../ProcessSupport/NativeMetalControl.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#import <xpc/xpc.h>

extern int LSBMetalNativeProbe(const char *absoluteFolder);
extern void xpc_connection_get_audit_token(xpc_connection_t connection, audit_token_t *token);
extern void xpc_connection_enable_sim2host_4sim(xpc_connection_t connection);
extern mach_port_t xpc_endpoint_copy_listener_port_4sim(xpc_endpoint_t endpoint);

static const LSBNativeMetalControl *authorization;
static xpc_connection_t listener;
static dispatch_source_t revocationTimer;
static NSHashTable *peers;
static uint64_t admittedGeneration;
static BOOL retired;
static void (*acceptPeer)(xpc_connection_t);
static void *implementation;

static void cancelPeers(void) {
    for (xpc_connection_t peer in peers) xpc_connection_cancel(peer);
    [peers removeAllObjects];
}

static BOOL readAuthorization(LSBNativeMetalAuthorization *snapshot) {
    int error = LSBNativeMetalControlRead(authorization, snapshot);
    // A helper keeps its original policy for its lifetime: one dedicated owner or all clients of the public session service.
    // Stock processRef state is process-global and peer invalidation is asynchronous; do not repurpose a live helper.
    if (admittedGeneration && (error || !snapshot->authorized || snapshot->generation != admittedGeneration)) retired = YES;
    if (retired || error || !snapshot->authorized) { cancelPeers(); return NO; }
    admittedGeneration = snapshot->generation;
    return YES;
}

__attribute__((visibility("default")))
int LSBMetalNativeServicePrepare(const char *absoluteFolder, int controlFD, mach_port_t *registrationPort) {
    if (!registrationPort || listener || authorization) return EINVAL;
    *registrationPort = MACH_PORT_NULL;
    int error = LSBNativeMetalControlImport(controlFD, &authorization);
    if (error) return error;
    error = LSBMetalNativeProbe(absoluteFolder);
    if (error) return error;
    @autoreleasepool {
        char path[PATH_MAX];
        int length = snprintf(path, sizeof(path), "%s/MTLSimImplementation.dylib", absoluteFolder);
        if (length < 0 || (size_t)length >= sizeof(path)) return ENAMETOOLONG;
        implementation = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!implementation) { fprintf(stderr, "[metal-native] service load failed: %s\n", dlerror()); return ENOEXEC; }
        void (*initialize)(xpc_connection_t, uint64_t, uint64_t) = dlsym(implementation, "init_with_xpc_connection");
        acceptPeer = dlsym(implementation, "LSBMTLSimulatorAcceptPeer");
        Dl_info initImage = {0}, acceptImage = {0};
        if (!initialize || !acceptPeer || !dladdr((void *)initialize, &initImage) || !dladdr((void *)acceptPeer, &acceptImage) ||
            initImage.dli_fbase != acceptImage.dli_fbase) return ENOEXEC;
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return ENODEV;
        dispatch_queue_t queue = dispatch_queue_create("LiveSimBooter.NativeMetal", DISPATCH_QUEUE_SERIAL);
        peers = [NSHashTable weakObjectsHashTable];
        listener = xpc_connection_create(NULL, queue);
        if (!listener) return ENOMEM;
        xpc_connection_enable_sim2host_4sim(listener);
        // The stock initializer installs its handler and activates the listener. It is still private until READY publishes the endpoint.
        initialize(listener, device.registryID, 0);
        xpc_connection_set_event_handler(listener, ^(xpc_object_t event) {
            if (xpc_get_type(event) != XPC_TYPE_CONNECTION) return;
            audit_token_t identity = {{0}};
            xpc_connection_get_audit_token(event, &identity);
            LSBNativeMetalAuthorization snapshot;
            if (!readAuthorization(&snapshot) || (!snapshot.publicAccess && memcmp(&identity, &snapshot.audit, sizeof(identity))) ||
                peers.allObjects.count >= (snapshot.publicAccess ? 256 : 64)) {
                fprintf(stderr, "[metal-native] rejected native=%u\n", identity.val[5]);
                xpc_connection_set_event_handler(event, ^(xpc_object_t ignored) { (void)ignored; });
                xpc_connection_activate(event);
                xpc_connection_cancel(event);
                return;
            }
            [peers addObject:event];
            fprintf(stderr, "[metal-native] accepted native=%u version=%u generation=%llu mode=%s\n", identity.val[5], identity.val[7],
                    (unsigned long long)snapshot.generation, snapshot.publicAccess ? "public" : "dedicated");
            acceptPeer(event);
        });
        // The broker revokes dedicated helpers on owner exit and the public helper on session exit. Either revocation cancels all peers.
        revocationTimer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
        if (!revocationTimer) return ENOMEM;
        dispatch_source_set_timer(revocationTimer, dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), 100 * NSEC_PER_MSEC, 10 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(revocationTimer, ^{ LSBNativeMetalAuthorization snapshot; (void)readAuthorization(&snapshot); });
        dispatch_resume(revocationTimer);
        xpc_endpoint_t endpoint = xpc_endpoint_create(listener);
        *registrationPort = endpoint ? xpc_endpoint_copy_listener_port_4sim(endpoint) : MACH_PORT_NULL;
        if (!MACH_PORT_VALID(*registrationPort)) return EIO;
        fprintf(stderr, "[metal-native] controlled service initialized registryID=0x%llx\n", (unsigned long long)device.registryID);
    }
    return 0;
}

__attribute__((visibility("default"))) void LSBMetalNativeServiceRun(void) { dispatch_main(); }
