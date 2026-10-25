#import "../Host/MetalDisplayRenderer.h"
#import <Metal/Metal.h>
#import <QuartzCore/CATransaction.h>
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

@interface LSBTestMetalLayer : CAMetalLayer
@property BOOL denyDrawable;
@property NSUInteger drawableRequests;
@property (strong) dispatch_semaphore_t drawableEntered;
@property (strong) dispatch_semaphore_t unblockDrawable;
@end

@implementation LSBTestMetalLayer
- (id<CAMetalDrawable>)nextDrawable {
    assert(!NSThread.isMainThread); // Drawable acquisition, including pool exhaustion, must never block main.
    ++_drawableRequests;
    dispatch_semaphore_t entered = self.drawableEntered, unblock = self.unblockDrawable;
    if (entered) {
        dispatch_semaphore_signal(entered);
        assert(!dispatch_semaphore_wait(unblock, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)));
    }
    return _denyDrawable ? nil : [super nextDrawable];
}
@end

enum { Width = 13, Height = 9, ImageBytes = Width * Height * 4 };

static IOSurfaceRef createSurface(size_t width, size_t height, uint32_t format) CF_RETURNS_RETAINED;
static IOSurfaceRef createSurface(size_t width, size_t height, uint32_t format) {
    NSDictionary *properties = @{(__bridge NSString *)kIOSurfaceWidth: @(width), (__bridge NSString *)kIOSurfaceHeight: @(height),
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4, (__bridge NSString *)kIOSurfacePixelFormat: @(format),
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, width * 4))};
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    assert(surface);
    return surface;
}

static void writeSurface(IOSurfaceRef surface, const uint8_t *pixels) {
    assert(!IOSurfaceLock(surface, 0, NULL));
    uint8_t *destination = IOSurfaceGetBaseAddress(surface);
    size_t stride = IOSurfaceGetBytesPerRow(surface), row = IOSurfaceGetWidth(surface) * 4, height = IOSurfaceGetHeight(surface);
    memset(destination, 0xDC, IOSurfaceGetAllocSize(surface));
    for (size_t y = 0; y < height; ++y) memcpy(destination + y * stride, pixels + y * row, row);
    assert(!IOSurfaceUnlock(surface, 0, NULL));
}

static void poisonSurface(IOSurfaceRef surface, uint8_t poison) {
    assert(!IOSurfaceLock(surface, 0, NULL));
    memset(IOSurfaceGetBaseAddress(surface), poison, IOSurfaceGetAllocSize(surface));
    assert(!IOSurfaceUnlock(surface, 0, NULL));
}

static void patchPixels(uint8_t pixels[ImageBytes], CGRect rect, const uint8_t color[4]) {
    for (unsigned y = (unsigned)rect.origin.y; y < rect.origin.y + rect.size.height; ++y)
        for (unsigned x = (unsigned)rect.origin.x; x < rect.origin.x + rect.size.width; ++x) memcpy(pixels + (y * Width + x) * 4, color, 4);
}

static void assertImage(CGImageRef image, const uint8_t *pixels, size_t width, size_t height) {
    assert(image && CGImageGetWidth(image) == width && CGImageGetHeight(image) == height);
    assert(CGImageGetBitsPerPixel(image) == 32 && CGImageGetBytesPerRow(image) == width * 4);
    CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
    assert(data && (size_t)CFDataGetLength(data) == width * height * 4);
    assert(!memcmp(CFDataGetBytePtr(data), pixels, width * height * 4));
    CFRelease(data);
}

static void waitUntil(BOOL (^condition)(void)) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:3];
    while (!condition()) {
        assert(deadline.timeIntervalSinceNow > 0);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, false);
    }
}

static int presentResult(LSBMetalDisplayRenderer *renderer, IOSurfaceRef surface, CGRect rectangle) {
    __block BOOL completed = NO;
    __block int result = -1;
    [renderer presentSurface:surface dirtyRect:rectangle completion:^(int error) {
        assert(NSThread.isMainThread && !completed);
        result = error;
        completed = YES;
    }];
    waitUntil(^BOOL { return completed; });
    return result;
}

static CGImageRef copyImage(LSBMetalDisplayRenderer *renderer, int *error) CF_RETURNS_RETAINED;
static CGImageRef copyImage(LSBMetalDisplayRenderer *renderer, int *error) {
    __block BOOL completed = NO;
    __block CGImageRef retained = NULL;
    __block int result = -1;
    [renderer copyImageWithCompletion:^(CGImageRef image, int failure) {
        assert(NSThread.isMainThread && !completed);
        if (image) retained = CGImageRetain(image);
        result = failure;
        completed = YES;
    }];
    waitUntil(^BOOL { return completed; });
    *error = result;
    return retained;
}

static CGImageRef checkBaseline(LSBMetalDisplayRenderer *renderer, const uint8_t *pixels, size_t width, size_t height) CF_RETURNS_RETAINED;
static CGImageRef checkBaseline(LSBMetalDisplayRenderer *renderer, const uint8_t *pixels, size_t width, size_t height) {
    int error = -1;
    CGImageRef image = copyImage(renderer, &error);
    assert(!error && renderer.hasFrame && CGSizeEqualToSize(renderer.pixelSize, CGSizeMake(width, height)));
    assertImage(image, pixels, width, height);
    return image;
}

static void drainPresentation(void) {
    [CATransaction flush];
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, false);
}

static void present(LSBMetalDisplayRenderer *renderer, IOSurfaceRef surface, CGRect rect) {
    int error = presentResult(renderer, surface, rect);
    if (error) fprintf(stderr, "Unexpected Metal presentation error: %d (%s)\n", error, strerror(error));
    assert(!error);
    drainPresentation();
}

static void testAsyncLifecycle(void) {
    LSBTestMetalLayer *layer = [LSBTestMetalLayer layer];
    layer.frame = CGRectMake(0, 0, Width, Height);
    layer.drawableEntered = dispatch_semaphore_create(0);
    layer.unblockDrawable = dispatch_semaphore_create(0);
    int error;
    LSBMetalDisplayRenderer *renderer = [[LSBMetalDisplayRenderer alloc] initWithLayer:layer error:&error];
    assert(renderer && !error);
    IOSurfaceRef source = createSurface(Width, Height, 0x42475241), next = createSurface(Width, Height, 0x42475241);
    uint8_t pixels[ImageBytes];
    memset(pixels, 0x7A, sizeof(pixels));
    writeSurface(source, pixels); writeSurface(next, pixels);
    CGRect full = CGRectMake(0, 0, Width, Height);
    __block unsigned callbacks = 0, readbacks = 0;
    [renderer presentSurface:source dirtyRect:full completion:^(int result) {
        assert(NSThread.isMainThread && !callbacks && result == ECANCELED);
        ++callbacks;
    }];
    CFRelease(source); // Renderer owns the source while the worker is paused; a caller reference is not required.
    waitUntil(^BOOL { return dispatch_semaphore_wait(layer.drawableEntered, DISPATCH_TIME_NOW) == 0; });
    __block BOOL heartbeat = NO;
    dispatch_async(dispatch_get_main_queue(), ^{ heartbeat = YES; });
    waitUntil(^BOOL { return heartbeat; });
    assert(!callbacks && !renderer.hasFrame);
    assert(presentResult(renderer, next, full) == EAGAIN); // Main can handle input and reject excess work while drawable acquisition stalls.
    [renderer copyImageWithCompletion:^(CGImageRef image, int result) {
        assert(NSThread.isMainThread && !image && !readbacks && result == ECANCELED);
        ++readbacks;
    }];
    __block BOOL rejectedReadback = NO;
    [renderer copyImageWithCompletion:^(CGImageRef image, int result) {
        assert(!image && result == EAGAIN);
        rejectedReadback = YES;
    }];
    assert(rejectedReadback);
    [renderer reset];
    assert(!renderer.hasFrame && layer.hidden && CGSizeEqualToSize(renderer.pixelSize, CGSizeZero));
    assert(!callbacks && !readbacks && presentResult(renderer, next, full) == EAGAIN);
    dispatch_semaphore_signal(layer.unblockDrawable);
    waitUntil(^BOOL { return callbacks == 1 && readbacks == 1; });
    assert(!renderer.hasFrame && layer.hidden);
    layer.drawableEntered = nil; layer.unblockDrawable = nil;
    assert(!copyImage(renderer, &error) && error == ENODATA);
    assert(presentResult(renderer, next, CGRectMake(0, 0, 1, 1)) == ENODATA);
    present(renderer, next, full);
    poisonSurface(next, 0);
    CGImageRef image = checkBaseline(renderer, pixels, Width, Height);
    CGImageRelease(image);

    // Dropping the last caller reference cannot release the renderer or IOSurface during worker reads.
    layer.drawableEntered = dispatch_semaphore_create(0);
    layer.unblockDrawable = dispatch_semaphore_create(0);
    writeSurface(next, pixels);
    __weak LSBMetalDisplayRenderer *weakRenderer = renderer;
    __block BOOL abandonedCompleted = NO;
    [renderer presentSurface:next dirtyRect:full completion:^(int result) {
        assert(NSThread.isMainThread && !result && !abandonedCompleted);
        abandonedCompleted = YES;
    }];
    CFRelease(next);
    waitUntil(^BOOL { return dispatch_semaphore_wait(layer.drawableEntered, DISPATCH_TIME_NOW) == 0; });
    renderer = nil;
    assert(weakRenderer && !abandonedCompleted);
    dispatch_semaphore_signal(layer.unblockDrawable);
    waitUntil(^BOOL { return abandonedCompleted && !weakRenderer; });
    drainPresentation();
    puts("MetalDisplayRendererTests: main responsiveness, bounded async work, reset cancellation and in-flight lifetime passed");
}

static void testRenderer(void) {
    LSBTestMetalLayer *layer = [LSBTestMetalLayer layer];
    layer.frame = CGRectMake(0, 0, Width, Height);
    int error = -1;
    LSBMetalDisplayRenderer *renderer = [[LSBMetalDisplayRenderer alloc] initWithLayer:layer error:&error];
    assert(renderer && !error && !renderer.hasFrame && CGSizeEqualToSize(renderer.pixelSize, CGSizeZero));
    assert(layer.device && layer.pixelFormat == MTLPixelFormatBGRA8Unorm && !layer.framebufferOnly && layer.maximumDrawableCount == 3);
    assert(layer.hidden && !copyImage(renderer, &error) && error == ENODATA);
    IOSurfaceRef first = createSurface(Width, Height, 0x42475241), second = createSurface(Width, Height, 0x42475241);
    CGRect full = CGRectMake(0, 0, Width, Height), dirty = CGRectMake(3, 2, 5, 4), edge = CGRectMake(Width - 1, Height - 1, 1, 1);
    assert(presentResult(renderer, first, dirty) == ENODATA && !layer.drawableRequests);
    uint8_t original[ImageBytes], expected[ImageBytes], incoming[ImageBytes];
    for (unsigned y = 0; y < Height; ++y) for (unsigned x = 0; x < Width; ++x) {
        unsigned i = (y * Width + x) * 4;
        original[i] = (uint8_t)(x * 7); original[i + 1] = (uint8_t)(y * 11); original[i + 2] = (uint8_t)(x + y); original[i + 3] = 255;
    }
    writeSurface(first, original);
    present(renderer, first, full);
    assert(!layer.hidden);
    poisonSurface(first, 0); // Source pixels can be immediately overwritten after the asynchronous completion callback.
    CGImageRef initialImage = checkBaseline(renderer, original, Width, Height);
    memcpy(expected, original, sizeof(expected));

    const uint8_t firstColor[4] = {31, 73, 127, 255}, secondColor[4] = {92, 41, 17, 255};
    memset(incoming, 0xEE, sizeof(incoming)); patchPixels(incoming, dirty, firstColor); patchPixels(expected, dirty, firstColor);
    writeSurface(second, incoming);
    present(renderer, second, dirty);
    poisonSurface(second, 0);
    CGImageRef partialImage = checkBaseline(renderer, expected, Width, Height);
    assertImage(initialImage, original, Width, Height);

    memset(incoming, 0x99, sizeof(incoming)); patchPixels(incoming, edge, secondColor); patchPixels(expected, edge, secondColor);
    writeSurface(first, incoming);
    present(renderer, first, edge); // Alternate back to a reused surface; its poisoned outside pixels must not replace the prior frame.
    poisonSurface(first, 0);
    CGImageRef latest = checkBaseline(renderer, expected, Width, Height);
    CGImageRelease(latest);
    uint8_t partialExpected[ImageBytes]; memcpy(partialExpected, original, sizeof(original)); patchPixels(partialExpected, dirty, firstColor);
    assertImage(partialImage, partialExpected, Width, Height);

    CGRect invalid[] = {CGRectZero, CGRectMake(0, 0, 0, 1), CGRectMake(0, 0, 1, 0), CGRectMake(-1, 0, 1, 1),
        CGRectMake(0, -1, 1, 1), CGRectMake(Width, 0, 1, 1), CGRectMake(0, Height, 1, 1), CGRectMake(Width - 1, Height - 1, 2, 1),
        CGRectMake(0, 0, UINT32_MAX, 1), CGRectMake(0, 0, 1, UINT32_MAX), CGRectMake(0.5, 0, 1, 1), CGRectMake(0, 0, 1.5, 1),
        CGRectMake(NAN, 0, 1, 1), CGRectMake(0, 0, INFINITY, 1)};
    NSUInteger priorRequests = layer.drawableRequests;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i)
        assert(presentResult(renderer, first, invalid[i]) == EINVAL);
    IOSurfaceRef absent = NULL;
    assert(presentResult(renderer, absent, full) == EINVAL);
    IOSurfaceRef unsupported = createSurface(Width, Height, 0x52474241);
    assert(presentResult(renderer, unsupported, full) == ENOTSUP);
    CFRelease(unsupported);
    assert(layer.drawableRequests == priorRequests && !layer.hidden);
    latest = checkBaseline(renderer, expected, Width, Height); CGImageRelease(latest);

    // Temporary drawable exhaustion must not apply the incoming dirty pixels to the owned baseline.
    layer.denyDrawable = YES;
    memset(incoming, 0xBB, sizeof(incoming)); patchPixels(incoming, dirty, secondColor); writeSurface(second, incoming);
    assert(presentResult(renderer, second, dirty) == EAGAIN && layer.drawableRequests == priorRequests + 1);
    latest = checkBaseline(renderer, expected, Width, Height); CGImageRelease(latest);
    layer.denyDrawable = NO;
    present(renderer, second, dirty);
    patchPixels(expected, dirty, secondColor);
    latest = checkBaseline(renderer, expected, Width, Height); CGImageRelease(latest);

    IOSurfaceRef resized = createSurface(7, 5, 0x42475241);
    uint8_t resizedPixels[7 * 5 * 4]; memset(resizedPixels, 0x5B, sizeof(resizedPixels));
    writeSurface(resized, resizedPixels);
    assert(presentResult(renderer, resized, CGRectMake(0, 0, 1, 1)) == ENODATA);
    layer.denyDrawable = YES;
    CGSize oldDrawableSize = layer.drawableSize;
    assert(presentResult(renderer, resized, CGRectMake(0, 0, 7, 5)) == EAGAIN && CGSizeEqualToSize(layer.drawableSize, oldDrawableSize));
    latest = checkBaseline(renderer, expected, Width, Height); CGImageRelease(latest);
    layer.denyDrawable = NO;
    present(renderer, resized, CGRectMake(0, 0, 7, 5));
    poisonSurface(resized, 0); CFRelease(resized);
    latest = checkBaseline(renderer, resizedPixels, 7, 5); CGImageRelease(latest);

    [renderer reset];
    assert(!renderer.hasFrame && layer.hidden && CGSizeEqualToSize(renderer.pixelSize, CGSizeZero));
    assert(!copyImage(renderer, &error) && error == ENODATA);
    assert(presentResult(renderer, first, dirty) == ENODATA);
    writeSurface(first, original);
    present(renderer, first, full);
    CFRelease(first); CFRelease(second);
    latest = checkBaseline(renderer, original, Width, Height); CGImageRelease(latest);
    [renderer reset];
    assertImage(initialImage, original, Width, Height); assertImage(partialImage, partialExpected, Width, Height);
    CGImageRelease(initialImage); CGImageRelease(partialImage);
    puts("MetalDisplayRendererTests: real GPU copies, dirty accumulation, reuse, readback, reset and drawable recovery passed");
}

int main(void) {
    alarm(20);
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { puts("MetalDisplayRendererTests SKIPPED: no native Metal device available"); return 0; }
        fprintf(stderr, "MetalDisplayRendererTests device=%s\n", device.name.UTF8String);
        testAsyncLifecycle(); testRenderer();
    }
    return 0;
}
