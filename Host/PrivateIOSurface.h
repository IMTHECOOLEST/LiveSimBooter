#pragma once

#import <Foundation/Foundation.h>
#import <xpc/xpc.h>

// Native IOSurface.framework interface. The host keeps its native cache and libxpc for this service.
@interface IOSurfaceRemoteServer : NSObject
- (instancetype)initWithListener:(xpc_connection_t)listener options:(NSDictionary *)options;
- (void)shutdown;
- (void)_handleClientConnection:(xpc_connection_t)connection;
@property (nonatomic, readonly) dispatch_queue_t queue;
@end

extern void xpc_connection_get_audit_token(xpc_connection_t connection, audit_token_t *token);
extern void xpc_connection_enable_sim2host_4sim(xpc_connection_t connection);
extern mach_port_t xpc_endpoint_copy_listener_port_4sim(xpc_endpoint_t endpoint);
