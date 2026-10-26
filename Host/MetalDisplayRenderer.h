#pragma once

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <QuartzCore/CAMetalLayer.h>

NS_ASSUME_NONNULL_BEGIN

// Native host only. All public entry points and completion callbacks run on the main thread.
// The renderer owns the layer's Metal configuration, hidden state, and drawableSize; the view owns its frame.
@interface LSBMetalDisplayRenderer : NSObject
@property (nonatomic, readonly) BOOL hasFrame;
@property (nonatomic, readonly) CGSize pixelSize;
// Default YES. Hidden presentation keeps consuming dirty updates into the owned baseline without requesting a drawable.
// Reenabling asynchronously presents that baseline; it never rereads a producer surface. Main-thread only.
@property (nonatomic, getter=isPresentationEnabled) BOOL presentationEnabled;

- (nullable instancetype)initWithLayer:(CAMetalLayer *)layer error:(int * _Nullable)error NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;
+ (instancetype)new NS_UNAVAILABLE;

// Retain a complete, nonplanar BGRA surface until completion. The caller must prevent producer writes until that callback.
// Copies only dirtyRect into a private baseline, then, when enabled, into CAMetalLayer's bounded three-drawable pool.
// Drawable acquisition and GPU completion waits run on a serial worker. The main thread never waits for those operations.
// Completion reports an errno value after all GPU reads end; only then may the caller release the producer fence/reuse the surface.
// One presentation may be outstanding; overlapping requests complete with EAGAIN. Immediate validation errors can complete inline.
// The first frame, and the first frame after reset or a size change, must cover the complete surface.
// Validation/allocation/drawable failures preserve the previous baseline. A submitted GPU write failure invalidates it and hides the layer.
- (void)presentSurface:(IOSurfaceRef)surface dirtyRect:(CGRect)dirtyRect completion:(void (^)(int error))completion;

// Blocking test/diagnostic convenience. Production presentation must use the completion-based method above.
- (int)presentSurface:(IOSurfaceRef)surface dirtyRect:(CGRect)dirtyRect;

// Immediately hides presentation and revokes the generation. Pending work retires GPU reads, then completes with ECANCELED.
// The view must detach this layer before showing a replacement session; previously submitted drawables may still be in Core Animation.
- (void)reset;

// Explicit diagnostics only: asynchronous GPU readback of the owned baseline. The image is borrowed for the duration of completion.
// Retain it inside the callback if later work needs it. One readback may be pending; reset revokes it with ECANCELED.
- (void)copyImageWithCompletion:(void (^)(CGImageRef _Nullable image, int error))completion;

// Blocking test/diagnostic convenience. Normal presentation never reads pixels on CPU.
- (nullable CGImageRef)copyImageWithError:(int * _Nullable)error CF_RETURNS_RETAINED;
@end

NS_ASSUME_NONNULL_END
