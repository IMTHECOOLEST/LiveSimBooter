#import "MetalDisplayRenderer.h"
#import <Metal/Metal.h>
#import <QuartzCore/CATransaction.h>
#include "SimulatorDisplayGeometry.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const size_t LSBMetalMaximumDimension = 16384;
typedef struct { int error; CGSize pixelSize; BOOL hasFrame; uint64_t presentedEpoch; } LSBMetalResult;

static int LSBMetalSurfaceLayout(IOSurfaceRef surface, CGRect rect, size_t *width, size_t *height, BOOL *fullFrame) {
    if (!surface) return EINVAL;
    if (IOSurfaceGetPixelFormat(surface) != 0x42475241 || IOSurfaceGetPlaneCount(surface) || IOSurfaceGetBytesPerElement(surface) != 4 ||
        IOSurfaceGetElementWidth(surface) != 1 || IOSurfaceGetElementHeight(surface) != 1) return ENOTSUP;
    *width = IOSurfaceGetWidth(surface);
    *height = IOSurfaceGetHeight(surface);
    size_t rowBytes, totalBytes;
    int error = LSBDisplayBGRALayout(*width, *height, IOSurfaceGetBytesPerRow(surface), IOSurfaceGetAllocSize(surface), &rowBytes, &totalBytes);
    if (error) return error;
    if (*width > LSBMetalMaximumDimension || *height > LSBMetalMaximumDimension) return ENOTSUP;
    CGFloat values[] = {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
    for (unsigned i = 0; i < 4; ++i)
        if (!isfinite(values[i]) || values[i] < 0 || values[i] != floor(values[i]) || values[i] > UINT32_MAX) return EINVAL;
    size_t x = (size_t)values[0], y = (size_t)values[1], changedWidth = (size_t)values[2], changedHeight = (size_t)values[3];
    if (!changedWidth || !changedHeight || x >= *width || y >= *height || changedWidth > *width - x || changedHeight > *height - y) return EINVAL;
    *fullFrame = !x && !y && changedWidth == *width && changedHeight == *height;
    return 0;
}

static void LSBMetalReleaseReadback(void *info, const void *bytes, size_t length) {
    (void)info;
    (void)length;
    free((void *)bytes);
}

@interface LSBMetalDisplayRenderer () {
    CAMetalLayer *_layer;
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    dispatch_queue_t _worker;
    atomic_uint_fast64_t _generation;
    atomic_uint_fast64_t _presentationEpoch;
    atomic_bool _presentationEnabled;
    // Only the serial render worker accesses GPU baseline and timing state.
    id<MTLTexture> _baseline;
    uint64_t _timingStart, _timingFrames, _timingDrawableNS, _timingGPUNS, _timingMaxDrawableNS, _timingMaxGPUNS;
    unsigned _slowLogs;
    // Only main accesses published metadata and enqueue bounds.
    CGSize _pixelSize;
    BOOL _hasFrame, _presenting, _readingBack, _refreshing, _refreshRequested;
    uint64_t _presentedEpoch;
}
@end

@implementation LSBMetalDisplayRenderer
@synthesize hasFrame = _hasFrame;
@synthesize pixelSize = _pixelSize;

- (instancetype)initWithLayer:(CAMetalLayer *)layer error:(int *)error {
    if (error) *error = 0;
    if (!NSThread.isMainThread || !layer) { if (error) *error = !NSThread.isMainThread ? EPERM : EINVAL; return nil; }
    if (!(self = [super init])) { if (error) *error = ENOMEM; return nil; }
    _device = MTLCreateSystemDefaultDevice();
    if (!_device) { if (error) *error = ENODEV; return nil; }
    _queue = [_device newCommandQueue];
    if (!_queue) { if (error) *error = ENOMEM; return nil; }
    _worker = dispatch_queue_create("com.kdt.LiveSimBooter.framebuffer-metal", DISPATCH_QUEUE_SERIAL_WITH_AUTORELEASE_POOL);
    atomic_init(&_generation, 1);
    atomic_init(&_presentationEpoch, 1);
    atomic_init(&_presentationEnabled, true);
    _queue.label = @"LiveSimBooter framebuffer copies";
    _layer = layer;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _layer.device = _device;
    _layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    _layer.framebufferOnly = NO;
    _layer.maximumDrawableCount = 3;
    _layer.allowsNextDrawableTimeout = YES;
    _layer.presentsWithTransaction = NO;
    _layer.opaque = YES;
    _layer.hidden = YES;
    CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    _layer.colorspace = colorSpace;
    if (colorSpace) CGColorSpaceRelease(colorSpace);
    [CATransaction commit];
    fprintf(stderr, "[framebuffer] native Metal renderer device=%s drawables=3\n", _device.name.UTF8String);
    return self;
}

- (void)setPresentationHidden:(BOOL)hidden {
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _layer.hidden = hidden;
    [CATransaction commit];
}

- (BOOL)isPresentationEnabled { return atomic_load_explicit(&_presentationEnabled, memory_order_acquire); }

- (BOOL)canPresentEpoch:(uint64_t)epoch {
    return self.isPresentationEnabled && epoch == atomic_load_explicit(&_presentationEpoch, memory_order_acquire);
}

- (void)updatePresentationHidden {
    [self setPresentationHidden:!_hasFrame || ![self canPresentEpoch:_presentedEpoch]];
}

- (void)setPresentationEnabled:(BOOL)enabled {
    NSAssert(NSThread.isMainThread, @"Metal display must be used on the main thread");
    if (!NSThread.isMainThread || self.isPresentationEnabled == enabled) return;
    atomic_fetch_add_explicit(&_presentationEpoch, 1, memory_order_acq_rel);
    atomic_store_explicit(&_presentationEnabled, enabled, memory_order_release);
    [self setPresentationHidden:YES];
    if (enabled) [self refreshPresentation];
    else _refreshRequested = NO;
}

- (void)reset {
    NSAssert(NSThread.isMainThread, @"Metal display must be used on the main thread");
    if (!NSThread.isMainThread) return;
    atomic_fetch_add_explicit(&_generation, 1, memory_order_release);
    [self setPresentationHidden:YES];
    _pixelSize = CGSizeZero;
    _hasFrame = NO;
    _presentedEpoch = 0;
    _refreshRequested = NO;
    dispatch_async(_worker, ^{
        self->_baseline = nil;
        self->_timingStart = self->_timingFrames = self->_timingDrawableNS = self->_timingGPUNS = 0;
        self->_timingMaxDrawableNS = self->_timingMaxGPUNS = self->_slowLogs = 0;
    });
}

- (BOOL)isCurrentGeneration:(uint64_t)generation {
    return generation == atomic_load_explicit(&_generation, memory_order_acquire);
}

- (LSBMetalResult)workerResult:(int)error {
    return (LSBMetalResult){error, _baseline ? CGSizeMake(_baseline.width, _baseline.height) : CGSizeZero, _baseline != nil, 0};
}

- (void)recordDrawableNS:(uint64_t)drawableNS gpuNS:(uint64_t)gpuNS started:(uint64_t)started {
    if (!_timingStart) _timingStart = started;
    ++_timingFrames;
    _timingDrawableNS += drawableNS;
    _timingGPUNS += gpuNS;
    if (drawableNS > _timingMaxDrawableNS) _timingMaxDrawableNS = drawableNS;
    if (gpuNS > _timingMaxGPUNS) _timingMaxGPUNS = gpuNS;
    if ((drawableNS > 50000000 || gpuNS > 50000000) && _slowLogs++ < 8)
        fprintf(stderr, "[framebuffer] slow Metal frame drawable_ms=%.2f gpu_ms=%.2f\n", drawableNS / 1e6, gpuNS / 1e6);
    if (!(_timingFrames % 300)) {
        uint64_t elapsed = clock_gettime_nsec_np(CLOCK_MONOTONIC) - _timingStart;
        fprintf(stderr, "[framebuffer] Metal frames=%llu fps=%.1f drawable_ms=%.3f gpu_ms=%.3f max_drawable_ms=%.2f max_gpu_ms=%.2f\n",
            (unsigned long long)_timingFrames, elapsed ? _timingFrames * 1e9 / elapsed : 0,
            _timingDrawableNS / (1e6 * _timingFrames), _timingGPUNS / (1e6 * _timingFrames),
            _timingMaxDrawableNS / 1e6, _timingMaxGPUNS / 1e6);
    }
}

- (LSBMetalResult)copySurfaceOnWorker:(IOSurfaceRef)surface dirtyRect:(CGRect)dirtyRect generation:(uint64_t)generation {
    @autoreleasepool {
        if (![self isCurrentGeneration:generation]) return [self workerResult:ECANCELED];
        size_t width, height;
        BOOL fullFrame;
        int error = LSBMetalSurfaceLayout(surface, dirtyRect, &width, &height, &fullFrame);
        if (error) return [self workerResult:error];
        BOOL sameSize = _baseline && _baseline.width == width && _baseline.height == height;
        if (!fullFrame && !sameSize) return [self workerResult:ENODATA];

        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                            width:width height:height mipmapped:NO];
        descriptor.storageMode = MTLStorageModeShared;
        descriptor.usage = MTLTextureUsageShaderRead;
        __attribute__((objc_precise_lifetime)) id<MTLTexture> source = [_device newTextureWithDescriptor:descriptor iosurface:surface plane:0];
        if (!source) return [self workerResult:ENOTSUP];
        source.label = @"LiveSimBooter borrowed framebuffer";
        id<MTLTexture> baseline = sameSize ? _baseline : nil;
        if (!baseline) {
            descriptor.storageMode = MTLStorageModePrivate;
            baseline = [_device newTextureWithDescriptor:descriptor];
            if (!baseline) return [self workerResult:ENOMEM];
            baseline.label = @"LiveSimBooter complete framebuffer baseline";
        }

        uint64_t epoch = atomic_load_explicit(&_presentationEpoch, memory_order_acquire);
        uint64_t drawableStart = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        BOOL requestedDrawable = [self canPresentEpoch:epoch];
        __attribute__((objc_precise_lifetime)) id<CAMetalDrawable> drawable = requestedDrawable ? [_layer nextDrawable] : nil;
        uint64_t drawableNS = clock_gettime_nsec_np(CLOCK_MONOTONIC) - drawableStart;
        if (![self isCurrentGeneration:generation]) return [self workerResult:ECANCELED];
        BOOL presenting = requestedDrawable && [self canPresentEpoch:epoch];
        id<MTLTexture> destination = presenting ? drawable.texture : nil;
        if (presenting) {
            if (!drawable) return [self workerResult:EAGAIN];
            if (destination.width != width || destination.height != height || destination.pixelFormat != MTLPixelFormatBGRA8Unorm)
                return [self workerResult:EINVAL];
        }
        id<MTLCommandBuffer> command = [_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!command || !blit) return [self workerResult:ENOMEM];
        command.label = presenting ? @"LiveSimBooter copy and present framebuffer" : @"LiveSimBooter retain hidden framebuffer";
        MTLOrigin origin = MTLOriginMake((NSUInteger)dirtyRect.origin.x, (NSUInteger)dirtyRect.origin.y, 0);
        MTLSize changed = MTLSizeMake((NSUInteger)dirtyRect.size.width, (NSUInteger)dirtyRect.size.height, 1);
        [blit copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:origin sourceSize:changed
                   toTexture:baseline destinationSlice:0 destinationLevel:0 destinationOrigin:origin];
        if (presenting) {
            [blit copyFromTexture:baseline sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                       toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
        [blit endEncoding];
        if (![self isCurrentGeneration:generation]) return [self workerResult:ECANCELED];
        presenting = presenting && [self canPresentEpoch:epoch];
        if (presenting) [command presentDrawable:drawable];
        uint64_t gpuStart = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        [command commit];
        [command waitUntilCompleted];
        uint64_t gpuNS = clock_gettime_nsec_np(CLOCK_MONOTONIC) - gpuStart;
        if (![self isCurrentGeneration:generation]) { _baseline = nil; return [self workerResult:ECANCELED]; }
        if (command.status != MTLCommandBufferStatusCompleted) {
            fprintf(stderr, "[framebuffer] Metal copy failed: %s\n", command.error.localizedDescription.UTF8String ?: "unknown command error");
            _baseline = nil; // A failed submitted write may have partly changed the old baseline; require a new full frame.
            return [self workerResult:EIO];
        }
        _baseline = baseline;
        [self recordDrawableNS:drawableNS gpuNS:gpuNS started:drawableStart];
        LSBMetalResult result = [self workerResult:0];
        result.presentedEpoch = presenting ? epoch : 0;
        return result;
    }
}

// The serial worker owns the baseline, so presentation-only reads cannot race with dirty updates or reset.
- (LSBMetalResult)presentBaselineOnWorker:(uint64_t)generation epoch:(uint64_t)epoch {
    @autoreleasepool {
        if (![self isCurrentGeneration:generation] || ![self canPresentEpoch:epoch]) return [self workerResult:ECANCELED];
        if (!_baseline) return [self workerResult:ENODATA];
        __attribute__((objc_precise_lifetime)) id<CAMetalDrawable> drawable = [_layer nextDrawable];
        if (![self isCurrentGeneration:generation] || ![self canPresentEpoch:epoch]) return [self workerResult:ECANCELED];
        if (!drawable) return [self workerResult:EAGAIN];
        id<MTLTexture> destination = drawable.texture;
        if (destination.width != _baseline.width || destination.height != _baseline.height || destination.pixelFormat != MTLPixelFormatBGRA8Unorm)
            return [self workerResult:EINVAL];
        id<MTLCommandBuffer> command = [_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!command || !blit) return [self workerResult:ENOMEM];
        command.label = @"LiveSimBooter present retained framebuffer";
        [blit copyFromTexture:_baseline sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(_baseline.width, _baseline.height, 1)
                   toTexture:destination destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        if (![self isCurrentGeneration:generation] || ![self canPresentEpoch:epoch]) return [self workerResult:ECANCELED];
        [command presentDrawable:drawable];
        [command commit];
        [command waitUntilCompleted];
        if (![self isCurrentGeneration:generation] || ![self canPresentEpoch:epoch]) return [self workerResult:ECANCELED];
        // This command only reads the baseline: an unavailable drawable or failed display copy must not discard accumulated dirty pixels.
        if (command.status != MTLCommandBufferStatusCompleted) return [self workerResult:EIO];
        LSBMetalResult result = [self workerResult:0];
        result.presentedEpoch = epoch;
        return result;
    }
}

- (void)refreshPresentation {
    if (!self.isPresentationEnabled) return;
    if (_refreshing) { _refreshRequested = YES; return; }
    _refreshing = YES;
    _refreshRequested = NO;
    uint64_t generation = atomic_load_explicit(&_generation, memory_order_acquire);
    uint64_t epoch = atomic_load_explicit(&_presentationEpoch, memory_order_acquire);
    dispatch_async(_worker, ^{
        LSBMetalResult result = [self presentBaselineOnWorker:generation epoch:epoch];
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_refreshing = NO;
            if ([self isCurrentGeneration:generation] && [self canPresentEpoch:epoch]) {
                if (!result.error) self->_presentedEpoch = result.presentedEpoch;
                [self updatePresentationHidden];
            }
            if (self->_refreshRequested) [self refreshPresentation];
        });
    });
}

- (int)beginSurface:(IOSurfaceRef)surface dirtyRect:(CGRect)dirtyRect oldSize:(CGSize *)oldSize generation:(uint64_t *)generation {
    if (!NSThread.isMainThread) return EPERM;
    if (_presenting) return EAGAIN;
    size_t width, height;
    BOOL fullFrame;
    int error = LSBMetalSurfaceLayout(surface, dirtyRect, &width, &height, &fullFrame);
    if (error) return error;
    *oldSize = _layer.drawableSize;
    *generation = atomic_load_explicit(&_generation, memory_order_acquire);
    _presenting = YES;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _layer.drawableSize = CGSizeMake(width, height);
    [CATransaction commit];
    return 0;
}

- (int)finishPresentation:(LSBMetalResult)result oldSize:(CGSize)oldSize generation:(uint64_t)generation {
    _presenting = NO;
    if (![self isCurrentGeneration:generation]) return ECANCELED;
    if (result.error) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        _layer.drawableSize = oldSize;
        [CATransaction commit];
    }
    _hasFrame = result.hasFrame;
    _pixelSize = result.pixelSize;
    if (result.presentedEpoch) _presentedEpoch = result.presentedEpoch;
    [self updatePresentationHidden];
    return result.error;
}

- (void)presentSurface:(IOSurfaceRef)surface dirtyRect:(CGRect)dirtyRect completion:(void (^)(int))completion {
    if (!NSThread.isMainThread) { dispatch_async(dispatch_get_main_queue(), ^{ completion(EPERM); }); return; }
    CGSize oldSize;
    uint64_t generation;
    int error = [self beginSurface:surface dirtyRect:dirtyRect oldSize:&oldSize generation:&generation];
    if (error) { completion(error); return; }
    IOSurfaceRef heldSurface = (IOSurfaceRef)CFRetain(surface);
    dispatch_async(_worker, ^{
        LSBMetalResult result = [self copySurfaceOnWorker:heldSurface dirtyRect:dirtyRect generation:generation];
        dispatch_async(dispatch_get_main_queue(), ^{
            int completionError = [self finishPresentation:result oldSize:oldSize generation:generation];
            completion(completionError);
            CFRelease(heldSurface);
        });
    });
}

- (int)presentSurface:(IOSurfaceRef)surface dirtyRect:(CGRect)dirtyRect {
    CGSize oldSize;
    uint64_t generation;
    int error = [self beginSurface:surface dirtyRect:dirtyRect oldSize:&oldSize generation:&generation];
    if (error) return error;
    IOSurfaceRef heldSurface = (IOSurfaceRef)CFRetain(surface);
    __block LSBMetalResult result;
    dispatch_sync(_worker, ^{ result = [self copySurfaceOnWorker:heldSurface dirtyRect:dirtyRect generation:generation]; });
    CFRelease(heldSurface);
    return [self finishPresentation:result oldSize:oldSize generation:generation];
}

- (CGImageRef)copyImageOnWorker:(int *)error CF_RETURNS_RETAINED {
    if (error) *error = 0;
    if (!_baseline) { if (error) *error = ENODATA; return NULL; }
    @autoreleasepool {
        size_t width = _baseline.width, height = _baseline.height, rowBytes = width * 4;
        // 256-byte pitch is accepted by both Apple and Mac GPU texture-to-buffer blits. Padding is excluded from the resulting image.
        if (rowBytes > SIZE_MAX - 255) { if (error) *error = EOVERFLOW; return NULL; }
        size_t stride = (rowBytes + 255) & ~(size_t)255;
        if (height > SIZE_MAX / stride || height > SIZE_MAX / rowBytes) { if (error) *error = EOVERFLOW; return NULL; }
        id<MTLBuffer> readback = [_device newBufferWithLength:stride * height options:MTLResourceStorageModeShared];
        if (!readback) { if (error) *error = ENOMEM; return NULL; }
        id<MTLCommandBuffer> command = [_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
        if (!command || !blit) { if (error) *error = ENOMEM; return NULL; }
        command.label = @"LiveSimBooter diagnostic framebuffer readback";
        [blit copyFromTexture:_baseline sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(width, height, 1)
                    toBuffer:readback destinationOffset:0 destinationBytesPerRow:stride destinationBytesPerImage:stride * height];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted || !readback.contents) { if (error) *error = EIO; return NULL; }
        size_t length = rowBytes * height;
        uint8_t *pixels = malloc(length);
        if (!pixels) { if (error) *error = ENOMEM; return NULL; }
        const uint8_t *source = readback.contents;
        for (size_t row = 0; row < height; ++row) memcpy(pixels + row * rowBytes, source + row * stride, rowBytes);
        CGDataProviderRef provider = CGDataProviderCreateWithData(NULL, pixels, length, LSBMetalReleaseReadback);
        if (!provider) { free(pixels); if (error) *error = ENOMEM; return NULL; }
        CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGImageRef image = colorSpace ? CGImageCreate(width, height, 8, 32, rowBytes, colorSpace,
            kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst, provider, NULL, false, kCGRenderingIntentDefault) : NULL;
        if (colorSpace) CGColorSpaceRelease(colorSpace);
        CGDataProviderRelease(provider);
        if (!image && error) *error = ENOMEM;
        return image;
    }
}

- (void)copyImageWithCompletion:(void (^)(CGImageRef, int))completion {
    if (!NSThread.isMainThread) { dispatch_async(dispatch_get_main_queue(), ^{ completion(NULL, EPERM); }); return; }
    if (_readingBack) { completion(NULL, EAGAIN); return; }
    _readingBack = YES;
    uint64_t generation = atomic_load_explicit(&_generation, memory_order_acquire);
    dispatch_async(_worker, ^{
        int error = ECANCELED;
        CGImageRef image = [self isCurrentGeneration:generation] ? [self copyImageOnWorker:&error] : NULL;
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_readingBack = NO;
            if ([self isCurrentGeneration:generation]) completion(image, error);
            else completion(NULL, ECANCELED);
            if (image) CGImageRelease(image);
        });
    });
}

- (CGImageRef)copyImageWithError:(int *)error {
    if (error) *error = 0;
    if (!NSThread.isMainThread) { if (error) *error = EPERM; return NULL; }
    if (_presenting || _readingBack) { if (error) *error = EAGAIN; return NULL; }
    __block CGImageRef image;
    __block int readError;
    dispatch_sync(_worker, ^{ image = [self copyImageOnWorker:&readError]; });
    if (error) *error = readError;
    return image;
}
@end
