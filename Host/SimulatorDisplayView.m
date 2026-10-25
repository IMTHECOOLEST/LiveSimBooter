#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>
#include "SimulatorDisplayGeometry.h"
#include <stdlib.h>
#include <string.h>

static void LSBReleaseDisplayPixels(void *info, const void *data, size_t size) {
    (void)info;
    (void)size;
    free((void *)data);
}

static CGImageRef LSBCopySurfaceImage(IOSurfaceRef surface, int *error) CF_RETURNS_RETAINED;
static CGImageRef LSBCopySurfaceImage(IOSurfaceRef surface, int *error) {
    *error = 0;
    if (!surface) { *error = EINVAL; return NULL; }
    if (IOSurfaceGetPixelFormat(surface) != 0x42475241 || IOSurfaceGetPlaneCount(surface) != 0 ||
        IOSurfaceGetBytesPerElement(surface) != 4 || IOSurfaceGetElementWidth(surface) != 1 || IOSurfaceGetElementHeight(surface) != 1) {
        *error = ENOTSUP;
        return NULL;
    }
    size_t width = IOSurfaceGetWidth(surface), height = IOSurfaceGetHeight(surface), stride = IOSurfaceGetBytesPerRow(surface);
    size_t rowBytes, copyBytes;
    *error = LSBDisplayBGRALayout(width, height, stride, IOSurfaceGetAllocSize(surface), &rowBytes, &copyBytes);
    if (*error) return NULL;
    uint8_t *pixels = malloc(copyBytes);
    if (!pixels) { *error = ENOMEM; return NULL; }
    if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL)) { free(pixels); *error = EIO; return NULL; }
    const uint8_t *source = IOSurfaceGetBaseAddress(surface);
    if (source) {
        for (size_t row = 0; row < height; ++row) memcpy(pixels + row * rowBytes, source + row * stride, rowBytes);
    } else *error = EINVAL;
    if (IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL)) *error = EIO;
    if (*error) { free(pixels); return NULL; }
    CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, copyBytes, LSBReleaseDisplayPixels);
    if (!provider) { free(pixels); *error = ENOMEM; return NULL; }
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef image = colorSpace ? CGImageCreate(width, height, 8, 32, rowBytes, colorSpace,
        kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst, provider, NULL, false, kCGRenderingIntentDefault) : NULL;
    if (colorSpace) CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    if (!image) *error = ENOMEM;
    return image;
}

// The portable tests exercise the actual IOSurface snapshot code above without linking UIKit on macOS.
#ifndef LSB_SIMULATOR_DISPLAY_CORE_ONLY
#import "SimulatorDisplayView.h"
#import "IndigoHIDServer.h"

@interface LSBSimulatorDisplayView () {
    UIImageView *_imageView;
    NSMapTable<UITouch *, NSNumber *> *_touches;
    CGSize _pixelSize;
    CGRect _viewport;
    uint64_t _nextTouchIdentifier;
    BOOL _applicationActive;
}
@end

@implementation LSBSimulatorDisplayView
@synthesize indigoServer = _indigoServer;

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
    _imageView = [[UIImageView alloc] initWithFrame:CGRectZero];
    _imageView.userInteractionEnabled = NO;
    [self addSubview:_imageView];
    _touches = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsStrongMemory | NSPointerFunctionsObjectPointerPersonality
                                    valueOptions:NSPointerFunctionsStrongMemory];
    _applicationActive = UIApplication.sharedApplication.applicationState == UIApplicationStateActive;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(applicationInactive:) name:UIApplicationWillResignActiveNotification object:nil];
    [center addObserver:self selector:@selector(applicationInactive:) name:UIApplicationDidEnterBackgroundNotification object:nil];
    [center addObserver:self selector:@selector(applicationActive:) name:UIApplicationDidBecomeActiveNotification object:nil];
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

- (int)presentSurface:(IOSurfaceRef)surface {
    if (!NSThread.isMainThread) return EPERM;
    if (!surface) {
        [self cancelActiveTouches];
        _imageView.image = nil;
        _pixelSize = CGSizeZero;
        [self setNeedsLayout];
        [self layoutIfNeeded];
        return 0;
    }
    int error;
    CGImageRef image = LSBCopySurfaceImage(surface, &error);
    if (!image) return error;
    UIImage *snapshot = [UIImage imageWithCGImage:image scale:1 orientation:UIImageOrientationUp];
    CGSize size = CGSizeMake(CGImageGetWidth(image), CGImageGetHeight(image));
    CGImageRelease(image);
    if (!snapshot) return ENOMEM;
    if (!CGSizeEqualToSize(size, _pixelSize)) [self cancelActiveTouches];
    _pixelSize = size;
    _imageView.image = snapshot;
    [self setNeedsLayout];
    [self layoutIfNeeded];
    return 0;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect viewport = LSBDisplayViewport(self.bounds, _pixelSize);
    if (!CGRectEqualToRect(viewport, _viewport)) [self cancelActiveTouches];
    _viewport = viewport;
    _imageView.frame = viewport;
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
    if (!_indigoServer || !_imageView.image || !_applicationActive || !self.window || self.hidden || !self.userInteractionEnabled) return;
    [self layoutIfNeeded];
    for (UITouch *touch in touches) {
        if (touch.view != self || [_touches objectForKey:touch] || _touches.count == 16) continue;
        CGPoint normalized;
        if (!LSBDisplayNormalize(_viewport, [touch locationInView:self], true, &normalized)) continue;
        uint64_t identifier = [self nextTouchIdentifier];
        if (![_indigoServer sendTouch:identifier phase:LSBIndigoTouchBegan x:normalized.x y:normalized.y]) {
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
