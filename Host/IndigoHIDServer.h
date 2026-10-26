#pragma once

#import <Foundation/Foundation.h>
#include "IndigoHIDTypes.h"
#include <mach/mach.h>
#include <stdint.h>

typedef NS_ENUM(unsigned, LSBIndigoTouchPhase) {
    LSBIndigoTouchBegan, LSBIndigoTouchMoved, LSBIndigoTouchEnded, LSBIndigoTouchCancelled
};

// Native host only. Receives the guest's Indigo event port; never opens the device's IOHID event system.
// Methods serialize on an internal queue. The presentation view supplies only its own UIKit touch identifiers/coordinates.
@interface LSBIndigoHIDServer : NSObject
@property (nonatomic, readonly) mach_port_t registrationPort; // Borrowed send right; retain the server while registering it.
@property (nonatomic, readonly) pid_t connectedClientPID; // Actual kernel PID, zero until an authenticated guest checks in.
- (instancetype)initWithError:(int *)error;
// Set the broker-verified native backboardd identity before guest handoff. NULL revokes access and drops the event port.
- (void)authorizeClientWithAuditToken:(const audit_token_t *)audit;
// x/y are finite, normalized coordinates within the presented simulator image, independent of the host view's scale.
// Callers must exclude touches beginning outside that image. Subsequent coordinates are clamped to [0,1].
// Up to 16 simultaneous identifiers are tracked; only successful sends change their state. Returns errno values.
// EBUSY means an earlier cancel is waiting for the guest to drain its event queue; do not record a new UIKit touch then.
- (int)sendTouch:(uint64_t)identifier phase:(LSBIndigoTouchPhase)phase x:(double)x y:(double)y;
// Supply an origin only on begin. It is latched until end/cancel; later phases use the no-edge method above.
- (int)sendTouch:(uint64_t)identifier phase:(LSBIndigoTouchPhase)phase x:(double)x y:(double)y edge:(LSBIndigoTouchEdge)edge;
// EAGAIN schedules nonblocking retries until delivery or disconnection, so a disappearing view can discard its local touch records.
- (int)cancelTouches;
- (void)close;
@end
