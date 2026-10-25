#pragma once

#import <Foundation/Foundation.h>
#include <mach/mach.h>

NS_ASSUME_NONNULL_BEGIN
// Uses Apple's native IOSurfaceRemoteServer after authenticating the guest connection.
@interface LSBIOSurfaceServer : NSObject
@property (nonatomic, readonly) mach_port_t registrationPort; // Borrowed send right.
@property (nonatomic, readonly) pid_t connectedClientPID; // Last accepted native identity, zero after revocation.
- (nullable instancetype)initWithError:(int * _Nullable)error;
// FIFO asynchronous updates. NULL cancels all accepted clients; later connections need a new authorization.
- (void)authorizeClientWithAuditToken:(const audit_token_t * _Nullable)audit;
- (void)close;
@end
NS_ASSUME_NONNULL_END
