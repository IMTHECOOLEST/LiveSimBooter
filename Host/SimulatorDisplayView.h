#pragma once

#import <IOSurface/IOSurfaceRef.h>
#import <UIKit/UIKit.h>

@class LSBIndigoHIDServer;

NS_ASSUME_NONNULL_BEGIN

// Native host presentation only. All methods/properties must be used on the main thread.
// Each server should have one presentation view; this view forwards only its own UIKit touches.
@interface LSBSimulatorDisplayView : UIView
@property (nonatomic, strong, nullable) LSBIndigoHIDServer *indigoServer;
// Discard the composition baseline and retire its presentation layer. The next frame must be complete.
- (void)resetPresentation;
// Enqueue a GPU copy without blocking UIKit. The caller must prevent producer writes until completion, called once on main.
// Integral dirty rectangles merge into host-owned pixels. The first frame after reset needs the complete surface.
// Unsupported formats return ENOTSUP through completion; a submitted GPU failure invalidates the baseline.
- (void)presentSurface:(IOSurfaceRef)surface dirtyRect:(CGRect)dirtyRect completion:(void (^)(int error))completion;
@end

NS_ASSUME_NONNULL_END
