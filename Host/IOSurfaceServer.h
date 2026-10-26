#pragma once

#import <Foundation/Foundation.h>
#include <mach/mach.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN
// Public simulator endpoint backed by Apple's native IOSurfaceRemoteServer. Any client holding the endpoint may connect.
@interface LSBIOSurfaceServer : NSObject
@property (nonatomic, readonly) mach_port_t registrationPort; // Borrowed send right.
@property (nonatomic, readonly) pid_t connectedClientPID; // Last accepted native identity, zero after shutdown.
- (nullable instancetype)initWithError:(int * _Nullable)error;
- (void)close;
@end
NS_ASSUME_NONNULL_END
