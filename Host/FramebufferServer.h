#pragma once

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <CoreGraphics/CGGeometry.h>
#include <mach/mach.h>

NS_ASSUME_NONNULL_BEGIN
typedef void (^LSBFramebufferCompletion)(int error);
// Invoked on main with a borrowed, completed BGRA surface and identical source/destination dirty rectangle.
// Finish all source reads before invoking completion, exactly once, on any queue. The server retains the surface and producer fence
// until completion, including after cancellation or revocation. A successful full frame establishes the baseline.
typedef void (^LSBFramebufferPresent)(IOSurfaceRef surface, CGRect dirtyRect, LSBFramebufferCompletion completion);

@interface LSBFramebufferServer : NSObject
@property (nonatomic, readonly) mach_port_t registrationPort; // Borrowed send right; retain server during registration.
@property (nonatomic, readonly) pid_t connectedClientPID; // Native audit PID; zero until authenticated check-in.
// Configure before authorizing the client. Nil causes presentation requests to fail with ENOTSUP.
@property (atomic, copy, nullable) LSBFramebufferPresent frameHandler;
- (nullable instancetype)initWithError:(int * _Nullable)error;
// FIFO asynchronous updates; never waits for the main queue. NULL revokes all session and surface state.
- (void)authorizeClientWithAuditToken:(const audit_token_t * _Nullable)audit;
- (void)close;
@end
NS_ASSUME_NONNULL_END
