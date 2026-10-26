#import "IOSurfaceServer.h"
#import "PrivateIOSurface.h"
#import <IOSurface/IOSurfaceRef.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>

enum { LSBMaximumIOSurfacePeers = 64 };

@interface LSBIOSurfaceServer () {
    IOSurfaceRemoteServer *_server;
    xpc_connection_t _listener;
    dispatch_queue_t _queue;
    NSHashTable *_peers;
    atomic_bool _closed;
    _Atomic(pid_t) _clientPID;
    _Atomic(mach_port_t) _registration;
}
@end

@implementation LSBIOSurfaceServer
- (instancetype)initWithError:(int *)error {
    if (error) *error = 0;
    if (!(self = [super init])) { if (error) *error = ENOMEM; return nil; }
    _listener = xpc_connection_create(NULL, NULL);
    if (!_listener) { if (error) *error = ENOMEM; return nil; }
    // Match the simulator client's graph format before the framework activates the listener. Accepted peers inherit this mode.
    xpc_connection_enable_sim2host_4sim(_listener);
    _server = [[IOSurfaceRemoteServer alloc] initWithListener:_listener options:@{}];
    if (!_server || !_server.queue) { if (error) *error = ENOTSUP; [self close]; return nil; }
    _queue = _server.queue;
    // The stock framework owns live connections. Weak tracking does not retain retired peers indefinitely.
    _peers = [NSHashTable weakObjectsHashTable];
    if (!_peers) { if (error) *error = ENOMEM; [self close]; return nil; }
    // This is the public simulator service: any client holding its endpoint may use the framework's original implementation.
    // Override only the listener's accept handler to bound live connections and cancel them when this runtime shuts down.
    // Nothing can connect before the endpoint is published to simulator launchd.
    __weak LSBIOSurfaceServer *weakSelf = self;
    xpc_connection_set_event_handler(_listener, ^(xpc_object_t event) {
        if (xpc_get_type(event) != XPC_TYPE_CONNECTION) return;
        LSBIOSurfaceServer *owner = weakSelf;
        audit_token_t token = {{0}};
        xpc_connection_get_audit_token(event, &token);
        // NSHashTable.count can include zeroed weak entries; allObjects counts only live peers and permits reconnects.
        if (!owner || atomic_load(&owner->_closed) || owner->_peers.allObjects.count >= LSBMaximumIOSurfacePeers) {
            xpc_connection_set_event_handler(event, ^(xpc_object_t ignored) { (void)ignored; });
            xpc_connection_activate(event);
            xpc_connection_cancel(event);
            return;
        }
        [owner->_peers addObject:event];
        atomic_store(&owner->_clientPID, (pid_t)token.val[5]);
        fprintf(stderr, "[iosurface] accepted public native=%u version=%u\n", token.val[5], token.val[7]);
        [owner->_server _handleClientConnection:event];
    });
    xpc_endpoint_t endpoint = xpc_endpoint_create(_listener);
    mach_port_t registration = endpoint ? xpc_endpoint_copy_listener_port_4sim(endpoint) : MACH_PORT_NULL;
    if (!MACH_PORT_VALID(registration)) { if (error) *error = EIO; [self close]; return nil; }
    atomic_store(&_registration, registration);
    return self;
}

- (mach_port_t)registrationPort { return atomic_load(&_registration); }
- (pid_t)connectedClientPID { return atomic_load(&_closed) ? 0 : atomic_load(&_clientPID); }

- (void)close {
    if (atomic_exchange(&_closed, true)) return;
    mach_port_t registration = atomic_exchange(&_registration, MACH_PORT_NULL);
    if (MACH_PORT_VALID(registration)) mach_port_deallocate(mach_task_self(), registration);
    atomic_store(&_clientPID, 0);
    // Capture resources, not self: close also runs from dealloc and may finish on the framework's queue.
    IOSurfaceRemoteServer *server = _server;
    xpc_connection_t listener = _listener;
    NSHashTable *peers = _peers;
    if (_queue) dispatch_async(_queue, ^{
        for (xpc_connection_t peer in peers) xpc_connection_cancel(peer);
        [peers removeAllObjects];
        [server shutdown];
        if (!server && listener) xpc_connection_cancel(listener);
    });
    else if (server) [server shutdown];
    else if (listener) {
        xpc_connection_set_event_handler(listener, ^(xpc_object_t ignored) { (void)ignored; });
        xpc_connection_activate(listener);
        xpc_connection_cancel(listener);
    }
}

- (void)dealloc { [self close]; }
@end
