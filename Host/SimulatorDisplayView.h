#pragma once

#import <IOSurface/IOSurfaceRef.h>
#import <UIKit/UIKit.h>

@class LSBIndigoHIDServer;

NS_ASSUME_NONNULL_BEGIN

// Native host presentation only. All methods/properties must be used on the main thread.
// Each server should have one presentation view; this view forwards only its own UIKit touches.
@interface LSBSimulatorDisplayView : UIView
@property (nonatomic, strong, nullable) LSBIndigoHIDServer *indigoServer;
// Snapshot a completed, nonplanar 32BGRA surface using premultiplied sRGB pixels. NULL clears the display.
// The caller must keep the surface valid and avoid writing it during this call. No surface is retained afterward.
// Returns errno values; unsupported formats return ENOTSUP and failures preserve the previous image.
- (int)presentSurface:(IOSurfaceRef _Nullable)surface;
@end

NS_ASSUME_NONNULL_END
