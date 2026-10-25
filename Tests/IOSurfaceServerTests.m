#import "../Host/IOSurfaceServer.m"
#include <assert.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

extern mach_port_t xpc_dictionary_copy_mach_send(xpc_object_t dictionary, const char *key);

@interface LSBIOSurfaceServer (Testing)
- (xpc_endpoint_t)testEndpoint;
@end
@implementation LSBIOSurfaceServer (Testing)
- (xpc_endpoint_t)testEndpoint { return xpc_endpoint_create(_listener); }
@end

static audit_token_t ownAudit(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    return token;
}

static xpc_connection_t clientFor(LSBIOSurfaceServer *server) {
    xpc_connection_t client = xpc_connection_create_from_endpoint([server testEndpoint]);
    xpc_connection_set_event_handler(client, ^(xpc_object_t ignored) { (void)ignored; });
    xpc_connection_enable_sim2host_4sim(client);
    xpc_connection_activate(client);
    return client;
}

static xpc_object_t sendRequest(xpc_connection_t client, xpc_object_t message) {
    dispatch_semaphore_t complete = dispatch_semaphore_create(0);
    __block xpc_object_t result;
    xpc_connection_send_message_with_reply(client, message, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(xpc_object_t reply) {
        result = reply;
        dispatch_semaphore_signal(complete);
    });
    assert(!dispatch_semaphore_wait(complete, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)));
    return result;
}

static xpc_object_t request(xpc_connection_t client, uint64_t method) {
    xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(message, "Method", method);
    return sendRequest(client, message);
}

static xpc_object_t surfaceRequest(xpc_connection_t client, uint64_t method, uint32_t surfaceID) {
    xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(message, "Method", method);
    xpc_dictionary_set_uint64(message, "SurfaceID", surfaceID);
    xpc_dictionary_set_uint64(message, "Options", 0);
    xpc_object_t reply = sendRequest(client, message);
    assert(xpc_get_type(reply) == XPC_TYPE_DICTIONARY);
    assert(!xpc_dictionary_get_uint64(reply, "ErrorCode"));
    return reply;
}

static uint32_t testPixel(unsigned x, unsigned y) { return 0xff000000u | (x << 16) | (y << 8) | (x ^ y); }

static void testSurfaceAllocation(xpc_connection_t client) {
    // The iOS 26 guest serializes CF dictionaries with IOCFSerialize, through this same exported IOSurface helper.
    void (*serialize)(xpc_object_t, const char *, CFTypeRef) = dlsym(RTLD_DEFAULT, "_ioSurfaceSerializeToXPCDictionaryWithKey");
    assert(serialize);
    NSDictionary *properties = @{
        (__bridge NSString *)kIOSurfaceWidth: @64, (__bridge NSString *)kIOSurfaceHeight: @32,
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, 256)),
        (__bridge NSString *)kIOSurfacePixelFormat: @0x42475241
    };
    xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(message, "Method", 2);
    serialize(message, "PropertiesDictionary", (__bridge CFDictionaryRef)properties);
    xpc_object_t reply = sendRequest(client, message);
    if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY || xpc_dictionary_get_uint64(reply, "ErrorCode")) {
        char *description = xpc_copy_description(reply);
        fprintf(stderr, "IOSurface allocation reply: %s\n", description);
        free(description);
        description = xpc_copy_description(message);
        fprintf(stderr, "IOSurface allocation request: %s\n", description);
        free(description);
    }
    assert(xpc_get_type(reply) == XPC_TYPE_DICTIONARY && !xpc_dictionary_get_uint64(reply, "ErrorCode"));
    size_t stateSize = 0;
    const void *stateBytes = xpc_dictionary_get_data(reply, "SurfaceState", &stateSize);
    assert(stateBytes && stateSize >= 72);
    uint32_t state[18];
    memcpy(state, stateBytes, sizeof(state));
    // Guest _ioSurfaceSyncState copies 72 bytes: ID@4, allocation@8, width@12, height@16, stride@20, base offset@24.
    assert(state[1] && state[2] >= 8192 && state[3] == 64 && state[4] == 32 && state[5] >= 256 && state[7] == 0x42475241);
    xpc_object_t memory = xpc_dictionary_get_value(reply, "MemoryObject");
    assert(memory && xpc_get_type(memory) == XPC_TYPE_SHMEM);
    void *mapping = NULL;
    size_t mappingSize = xpc_shmem_map(memory, &mapping);
    assert(mapping && mappingSize >= state[6] && mappingSize - state[6] >= (size_t)state[5] * 32);

    surfaceRequest(client, 4, state[1]); // The guest's normal IOSurfaceLock request.
    uint8_t *pixels = (uint8_t *)mapping + state[6];
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 64; ++x) {
        uint32_t pixel = testPixel(x, y);
        memcpy(pixels + y * state[5] + x * 4, &pixel, sizeof(pixel));
    }
    surfaceRequest(client, 5, state[1]);

    xpc_object_t exported = surfaceRequest(client, 14, state[1]);
    mach_port_t port = xpc_dictionary_copy_mach_send(exported, "MachPort");
    assert(MACH_PORT_VALID(port));
    IOSurfaceRef surface = IOSurfaceLookupFromMachPort(port);
    assert(surface && IOSurfaceGetID(surface) == state[1]);
    assert(IOSurfaceGetWidth(surface) == 64 && IOSurfaceGetHeight(surface) == 32 && IOSurfaceGetPixelFormat(surface) == 0x42475241);
    assert(!IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL));
    const uint8_t *nativePixels = IOSurfaceGetBaseAddress(surface);
    assert(nativePixels && IOSurfaceGetBytesPerRow(surface) == state[5]);
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 64; ++x) {
        uint32_t pixel;
        memcpy(&pixel, nativePixels + y * state[5] + x * 4, sizeof(pixel));
        assert(pixel == testPixel(x, y));
    }
    assert(!IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL));
    CFRelease(surface);
    assert(!mach_port_deallocate(mach_task_self(), port));
    assert(!munmap(mapping, mappingSize));
    surfaceRequest(client, 3, state[1]); // Balance the remote create independently of the exported Mach right.
}

static void awaitDisconnected(xpc_connection_t client) {
    // Cancellation is asynchronous; an already queued native framework reply may arrive before connection invalidation.
    for (unsigned i = 0; i < 100; ++i) {
        if (xpc_get_type(request(client, 0)) == XPC_TYPE_ERROR) return;
        usleep(1000);
    }
    assert(!"IOSurface connection did not invalidate");
}

int main(void) {
    alarm(15);
    @autoreleasepool {
        int error = 42;
        LSBIOSurfaceServer *server = [[LSBIOSurfaceServer alloc] initWithError:&error];
        assert(server && !error && MACH_PORT_VALID(server.registrationPort));
        xpc_connection_t client = clientFor(server);
        assert(xpc_get_type(request(client, 0)) == XPC_TYPE_ERROR); // Unpublished client identity cannot allocate surfaces.
        assert(!server.connectedClientPID);
        xpc_connection_cancel(client);

        audit_token_t token = ownAudit();
        ++token.val[7];
        [server authorizeClientWithAuditToken:&token];
        client = clientFor(server);
        assert(xpc_get_type(request(client, 0)) == XPC_TYPE_ERROR);
        assert(!server.connectedClientPID);
        xpc_connection_cancel(client);

        token = ownAudit();
        [server authorizeClientWithAuditToken:&token];
        client = clientFor(server);
        // Method 0 is the stock simulator IOSurface client's initial limits handshake.
        xpc_object_t reply = request(client, 0);
        assert(xpc_get_type(reply) == XPC_TYPE_DICTIONARY);
        size_t length = 0;
        assert(xpc_dictionary_get_data(reply, "Limits", &length) && length >= 40);
        assert(server.connectedClientPID == getpid());
        testSurfaceAllocation(client);
        [server authorizeClientWithAuditToken:&token]; // Same identity must not disrupt its live connection.
        assert(xpc_get_type(request(client, 0)) == XPC_TYPE_DICTIONARY);
        [server authorizeClientWithAuditToken:NULL];
        for (unsigned i = 0; i < 500 && server.connectedClientPID; ++i) usleep(1000);
        assert(!server.connectedClientPID);
        awaitDisconnected(client);
        xpc_connection_cancel(client);

        [server authorizeClientWithAuditToken:&token];
        client = clientFor(server);
        assert(xpc_get_type(request(client, 0)) == XPC_TYPE_DICTIONARY);
        [server close];
        assert(!server.registrationPort && !server.connectedClientPID);
        awaitDisconnected(client);
        xpc_connection_cancel(client);
        [server close];
        server = nil;
        server = [[LSBIOSurfaceServer alloc] initWithError:&error];
        assert(server && !error);
        server = nil;
    }
    puts("IOSurfaceServer: stock limits, allocation, shared pixels, Mach-port export, audit authorization, revocation and shutdown passed");
    return 0;
}
