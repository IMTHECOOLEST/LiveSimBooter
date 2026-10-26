#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#import <ImageIO/ImageIO.h>
#include "SimulatorDisplayGeometry.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const LSBCaptureFilename = "LiveSimBooter-first-frame.png";
static BOOL LSBDisplayCaptureEnabled(const char *value) { return value && !strcmp(value, "1"); }

// Encode only the immutable snapshot, never the producer's IOSurface. Production supplies the fixed native app tmp directory.
static int LSBCaptureDisplayImage(CGImageRef image, const char *directory) {
    if (!image || !directory) return EINVAL;
    CFMutableDataRef encoded = CFDataCreateMutable(kCFAllocatorDefault, 0);
    if (!encoded) return ENOMEM;
    CGImageDestinationRef destination = CGImageDestinationCreateWithData(encoded, CFSTR("public.png"), 1, NULL);
    if (!destination) { CFRelease(encoded); return ENOMEM; }
    CGImageDestinationAddImage(destination, image, NULL);
    BOOL finished = CGImageDestinationFinalize(destination);
    CFRelease(destination);
    if (!finished) { CFRelease(encoded); return EIO; }
    int folder = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (folder < 0) { int error = errno; CFRelease(encoded); return error; }
    char temporary[96];
    snprintf(temporary, sizeof(temporary), ".LiveSimBooter-first-frame.%08x%08x.png", arc4random(), arc4random());
    int descriptor = openat(folder, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    int error = descriptor < 0 ? errno : 0;
    if (!error) {
        const UInt8 *bytes = CFDataGetBytePtr(encoded);
        size_t remaining = (size_t)CFDataGetLength(encoded);
        while (remaining) {
            ssize_t written = write(descriptor, bytes, remaining);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { error = written < 0 ? errno : EIO; break; }
            bytes += written;
            remaining -= (size_t)written;
        }
        if (close(descriptor) && !error) error = errno;
        // Atomic replacement also replaces a stale destination symlink rather than writing through it.
        if (!error && renameat(folder, temporary, folder, LSBCaptureFilename)) error = errno;
        if (error) unlinkat(folder, temporary, 0);
    }
    close(folder);
    CFRelease(encoded);
    return error;
}

// The portable tests exercise geometry and diagnostic PNG publication without linking UIKit on macOS.
#ifndef LSB_SIMULATOR_DISPLAY_CORE_ONLY
#import "SimulatorDisplayView.h"
#import "IndigoHIDServer.h"
#import "MetalDisplayRenderer.h"

@interface LSBSimulatorDisplayView () {
    CAMetalLayer *_displayLayer;
    LSBMetalDisplayRenderer *_renderer;
    int _rendererError;
    NSMapTable<UITouch *, NSNumber *> *_touches;
    CGSize _pixelSize;
    CGRect _viewport;
    uint64_t _nextTouchIdentifier, _presentationGeneration;
    BOOL _applicationActive, _captureFirstFrame, _captureAttempted, _presentationEnabled;
}
@end

@implementation LSBSimulatorDisplayView
@synthesize indigoServer = _indigoServer;
@synthesize presentationEnabled = _presentationEnabled;

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) [self initializeDisplay];
    return self;
}

- (instancetype)initWithCoder:(NSCoder *)coder {
    if ((self = [super initWithCoder:coder])) [self initializeDisplay];
    return self;
}

- (void)initializeDisplay {
    self.backgroundColor = UIColor.blackColor;
    self.opaque = YES;
    self.clipsToBounds = YES;
    self.multipleTouchEnabled = YES;
    _presentationEnabled = YES;
    _touches = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsStrongMemory | NSPointerFunctionsObjectPointerPersonality
                                    valueOptions:NSPointerFunctionsStrongMemory];
    _applicationActive = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    _captureFirstFrame = LSBDisplayCaptureEnabled(getenv("LSB_FRAMEBUFFER_CAPTURE_FIRST"));
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(applicationInactive:) name:UIApplicationWillResignActiveNotification object:nil];
    [center addObserver:self selector:@selector(applicationInactive:) name:UIApplicationDidEnterBackgroundNotification object:nil];
    [center addObserver:self selector:@selector(applicationActive:) name:UIApplicationDidBecomeActiveNotification object:nil];
}

- (int)prepareRenderer {
    if (_renderer) return 0;
    if (_rendererError) return _rendererError;
    CAMetalLayer *layer = [CAMetalLayer layer];
    LSBMetalDisplayRenderer *renderer = [[LSBMetalDisplayRenderer alloc] initWithLayer:layer error:&_rendererError];
    if (!renderer) {
        fprintf(stderr, "[framebuffer] Metal presenter initialization failed: %s\n", strerror(_rendererError));
        return _rendererError ?: ENODEV;
    }
    _displayLayer = layer;
    _renderer = renderer;
    renderer.presentationEnabled = _presentationEnabled;
    [self.layer addSublayer:layer];
    return 0;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
    if (_touches.count) [_indigoServer cancelTouches];
}

- (void)cancelActiveTouches {
    if (_touches.count) [_indigoServer cancelTouches];
    [_touches removeAllObjects];
}

- (void)setIndigoServer:(LSBIndigoHIDServer *)indigoServer {
    NSAssert(NSThread.isMainThread, @"Simulator display must be used on the main thread");
    if (_indigoServer == indigoServer) return;
    [self cancelActiveTouches];
    _indigoServer = indigoServer;
}

- (void)setPresentationEnabled:(BOOL)enabled {
    NSAssert(NSThread.isMainThread, @"Simulator display must be used on the main thread");
    if (!NSThread.isMainThread || _presentationEnabled == enabled) return;
    _presentationEnabled = enabled;
    if (!enabled) [self cancelActiveTouches];
    _renderer.presentationEnabled = enabled;
}

- (void)resetPresentation {
    NSAssert(NSThread.isMainThread, @"Simulator display must be used on the main thread");
    ++_presentationGeneration;
    [self cancelActiveTouches];
    [_renderer reset];
    // GPU completion is not display completion. Retire the layer as well as its composition baseline between sessions.
    [_displayLayer removeFromSuperlayer];
    _displayLayer = nil;
    _renderer = nil;
    _rendererError = 0;
    _pixelSize = CGSizeZero;
    [self setNeedsLayout];
    [self layoutIfNeeded];
}

- (void)captureFirstFrameWithRenderer:(LSBMetalDisplayRenderer *)renderer surfaceID:(uint32_t)surfaceID {
    if (!_captureFirstFrame || _captureAttempted) return;
    _captureAttempted = YES; // Only this explicitly enabled diagnostic reads pixels back to the CPU.
    NSString *directory = [NSHomeDirectory() stringByAppendingPathComponent:@"tmp"];
    [renderer copyImageWithCompletion:^(CGImageRef image, int error) {
        if (!image) {
            fprintf(stderr, "[framebuffer] first-frame Metal readback failed: %s\n", strerror(error));
            return;
        }
        CGImageRef captured = CGImageRetain(image);
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            @autoreleasepool {
                int captureError = LSBCaptureDisplayImage(captured, directory.fileSystemRepresentation);
                fprintf(stderr, "[framebuffer] first-frame PNG surface=%u size=%zux%zu error=%d path=%s/%s\n", surfaceID,
                    CGImageGetWidth(captured), CGImageGetHeight(captured), captureError, directory.fileSystemRepresentation, LSBCaptureFilename);
                CGImageRelease(captured);
            }
        });
    }];
}

- (void)presentSurface:(IOSurfaceRef)surface dirtyRect:(CGRect)dirtyRect completion:(void (^)(int))completion {
    if (!NSThread.isMainThread) { completion(EPERM); return; }
    int error = [self prepareRenderer];
    if (error) { completion(error); return; }
    LSBMetalDisplayRenderer *renderer = _renderer;
    uint64_t generation = _presentationGeneration;
    uint32_t surfaceID = surface ? IOSurfaceGetID(surface) : 0;
    // Only geometry changes run here. Drawable acquisition, GPU completion, and optional readback happen off the UI thread.
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _displayLayer.frame = surface ? LSBDisplayViewport(self.bounds, CGSizeMake(IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface))) : _viewport;
    [CATransaction commit];
    __weak LSBSimulatorDisplayView *weakSelf = self;
    [renderer presentSurface:surface dirtyRect:dirtyRect completion:^(int presentError) {
        LSBSimulatorDisplayView *view = weakSelf;
        if (!view || generation != view->_presentationGeneration || renderer != view->_renderer) { completion(ECANCELED); return; }
        if (!presentError) {
            CGSize size = renderer.pixelSize;
            if (!CGSizeEqualToSize(size, view->_pixelSize)) [view cancelActiveTouches];
            view->_pixelSize = size;
            [view setNeedsLayout];
            [view layoutIfNeeded];
            [view captureFirstFrameWithRenderer:renderer surfaceID:surfaceID];
        } else {
            if (!renderer.hasFrame) {
                [view cancelActiveTouches];
                view->_pixelSize = CGSizeZero;
            }
            [view setNeedsLayout];
            [view layoutIfNeeded]; // Restore the previous viewport after a rejected size or rectangle.
        }
        completion(presentError);
    }];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect viewport = LSBDisplayViewport(self.bounds, _pixelSize);
    if (!CGRectEqualToRect(viewport, _viewport)) [self cancelActiveTouches];
    _viewport = viewport;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _displayLayer.frame = viewport;
    [CATransaction commit];
}

- (void)willMoveToWindow:(UIWindow *)window {
    if (!window) [self cancelActiveTouches];
    [super willMoveToWindow:window];
}

- (void)willMoveToSuperview:(UIView *)superview {
    if (!superview) [self cancelActiveTouches];
    [super willMoveToSuperview:superview];
}

- (void)setHidden:(BOOL)hidden {
    if (hidden) [self cancelActiveTouches];
    [super setHidden:hidden];
}

- (void)setUserInteractionEnabled:(BOOL)enabled {
    if (!enabled) [self cancelActiveTouches];
    [super setUserInteractionEnabled:enabled];
}

- (void)applicationInactive:(NSNotification *)notification {
    (void)notification;
    _applicationActive = NO;
    [self cancelActiveTouches];
}

- (void)applicationActive:(NSNotification *)notification {
    (void)notification;
    _applicationActive = YES;
}

- (uint64_t)nextTouchIdentifier {
    // Zero is reserved; avoid reusing a live identifier even after the counter wraps.
    for (;;) {
        if (!++_nextTouchIdentifier) ++_nextTouchIdentifier;
        BOOL used = NO;
        for (NSNumber *identifier in _touches.objectEnumerator) if (identifier.unsignedLongLongValue == _nextTouchIdentifier) { used = YES; break; }
        if (!used) return _nextTouchIdentifier;
    }
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
    (void)event;
    if (!_indigoServer || !_renderer.hasFrame || !_presentationEnabled || !_applicationActive || !self.window || self.hidden ||
        !self.userInteractionEnabled) return;
    [self layoutIfNeeded];
    for (UITouch *touch in touches) {
        if (touch.view != self || [_touches objectForKey:touch] || _touches.count == 16) continue;
        CGPoint normalized;
        CGPoint location = [touch locationInView:self];
        if (!LSBDisplayNormalize(_viewport, location, true, &normalized)) continue;
        uint64_t identifier = [self nextTouchIdentifier];
        LSBIndigoTouchEdge edge = LSBDisplayBeginningEdge(_viewport, location);
        if (![_indigoServer sendTouch:identifier phase:LSBIndigoTouchBegan x:normalized.x y:normalized.y edge:edge]) {
            [_touches setObject:@(identifier) forKey:touch];
        }
    }
}

- (void)forwardTouches:(NSSet<UITouch *> *)touches phase:(LSBIndigoTouchPhase)phase {
    [self layoutIfNeeded];
    for (UITouch *touch in touches) {
        NSNumber *identifier = [_touches objectForKey:touch];
        if (!identifier) continue; // Letterbox starts remain ignored for their entire lifetime.
        CGPoint normalized;
        if (!LSBDisplayNormalize(_viewport, [touch locationInView:self], false, &normalized)) { [self cancelActiveTouches]; return; }
        int error = [_indigoServer sendTouch:identifier.unsignedLongLongValue phase:phase x:normalized.x y:normalized.y];
        if (error) {
            if (phase == LSBIndigoTouchMoved && error == EAGAIN) continue;
            // The server owns cancellation retries if the guest receive queue is currently full.
            [self cancelActiveTouches];
            return;
        }
        if (phase == LSBIndigoTouchEnded || phase == LSBIndigoTouchCancelled) [_touches removeObjectForKey:touch];
    }
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self forwardTouches:touches phase:LSBIndigoTouchMoved]; }
- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self forwardTouches:touches phase:LSBIndigoTouchEnded]; }
- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event { (void)event; [self forwardTouches:touches phase:LSBIndigoTouchCancelled]; }
@end
#endif
