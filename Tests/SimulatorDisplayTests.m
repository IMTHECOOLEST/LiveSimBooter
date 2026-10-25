#define LSB_SIMULATOR_DISPLAY_CORE_ONLY 1
#include "../Host/SimulatorDisplayView.m"
#include <assert.h>
#include <stdio.h>

static void testCoordinates(void) {
    CGRect viewport = LSBDisplayViewport(CGRectMake(0, 0, 300, 200), CGSizeMake(100, 200));
    assert(CGRectEqualToRect(viewport, CGRectMake(100, 0, 100, 200)));
    CGPoint point;
    assert(!LSBDisplayNormalize(viewport, CGPointMake(99, 100), true, &point));
    assert(LSBDisplayNormalize(viewport, CGPointMake(100, 0), true, &point) && point.x == 0 && point.y == 0);
    assert(LSBDisplayNormalize(viewport, CGPointMake(200, 200), true, &point) && point.x == 1 && point.y == 1);
    assert(LSBDisplayNormalize(viewport, CGPointMake(150, 50), true, &point) && point.x == 0.5 && point.y == 0.25);
    assert(LSBDisplayNormalize(viewport, CGPointMake(-10, 250), false, &point) && point.x == 0 && point.y == 1);
    viewport = LSBDisplayViewport(CGRectMake(10, 20, 200, 300), CGSizeMake(400, 200));
    assert(CGRectEqualToRect(viewport, CGRectMake(10, 120, 200, 100)));
    assert(!LSBDisplayNormalize(viewport, CGPointMake(100, 119), true, &point));
    assert(LSBDisplayNormalize(viewport, CGPointMake(110, 170), true, &point) && point.x == 0.5 && point.y == 0.5);
    assert(CGRectIsEmpty(LSBDisplayViewport(CGRectZero, CGSizeMake(10, 10))));
    assert(CGRectIsEmpty(LSBDisplayViewport(CGRectMake(0, 0, 10, 10), CGSizeMake(0, 10))));
    assert(CGRectIsEmpty(LSBDisplayViewport(CGRectMake(NAN, 0, 10, 10), CGSizeMake(10, 10))));
    assert(CGRectIsEmpty(LSBDisplayViewport(CGRectMake(0, 0, 10, 10), CGSizeMake(INFINITY, 10))));
    assert(!LSBDisplayNormalize(CGRectZero, CGPointZero, false, &point));
    assert(!LSBDisplayNormalize(viewport, CGPointMake(NAN, 0), false, &point));
    assert(!LSBDisplayNormalize(viewport, CGPointMake(0, INFINITY), true, &point));
}

static void testLayoutBounds(void) {
    size_t row, bytes;
    assert(!LSBDisplayBGRALayout(2, 3, 16, 40, &row, &bytes) && row == 8 && bytes == 24);
    assert(LSBDisplayBGRALayout(2, 3, 16, 39, &row, &bytes) == EINVAL);
    assert(LSBDisplayBGRALayout(0, 3, 16, 48, &row, &bytes) == EINVAL);
    assert(LSBDisplayBGRALayout(2, 0, 16, 48, &row, &bytes) == EINVAL);
    assert(LSBDisplayBGRALayout(2, 3, 7, 48, &row, &bytes) == EINVAL);
    assert(LSBDisplayBGRALayout(SIZE_MAX / 4 + 1, 1, SIZE_MAX, SIZE_MAX, &row, &bytes) == EOVERFLOW);
    assert(LSBDisplayBGRALayout(1, SIZE_MAX / 4 + 1, 4, SIZE_MAX, &row, &bytes) == EOVERFLOW);
    assert(LSBDisplayBGRALayout(1, 3, SIZE_MAX / 2, SIZE_MAX, &row, &bytes) == EOVERFLOW);
    assert(!LSBDisplayBGRALayout(1, 1, SIZE_MAX, 4, &row, &bytes) && row == 4 && bytes == 4);
}

static IOSurfaceRef createSurface(uint32_t format) CF_RETURNS_RETAINED;
static IOSurfaceRef createSurface(uint32_t format) {
    size_t stride = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, 16);
    NSDictionary *properties = @{(__bridge NSString *)kIOSurfaceWidth: @2, (__bridge NSString *)kIOSurfaceHeight: @2,
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4, (__bridge NSString *)kIOSurfaceBytesPerRow: @(stride),
        (__bridge NSString *)kIOSurfacePixelFormat: @(format)};
    return IOSurfaceCreate((__bridge CFDictionaryRef)properties);
}

static void testSurfaceSnapshot(void) {
    IOSurfaceRef surface = createSurface(0x42475241);
    assert(surface);
    size_t stride = IOSurfaceGetBytesPerRow(surface);
    assert(stride >= 16);
    const uint8_t expected[] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
    assert(!IOSurfaceLock(surface, 0, NULL));
    uint8_t *pixels = IOSurfaceGetBaseAddress(surface);
    memset(pixels, 0xCC, IOSurfaceGetAllocSize(surface));
    memcpy(pixels, expected, 8);
    memcpy(pixels + stride, expected + 8, 8);
    assert(!IOSurfaceUnlock(surface, 0, NULL));
    int error = 42;
    CGImageRef image = LSBCopySurfaceImage(surface, &error);
    assert(image && !error && CGImageGetWidth(image) == 2 && CGImageGetHeight(image) == 2 && CGImageGetBytesPerRow(image) == 8);
    assert(CGImageGetBitmapInfo(image) == (kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst));
    // A producer may reuse the surface after presentSurface returns; the snapshot must keep its own pixels.
    assert(!IOSurfaceLock(surface, 0, NULL));
    memset(pixels, 0, IOSurfaceGetAllocSize(surface));
    assert(!IOSurfaceUnlock(surface, 0, NULL));
    CFRelease(surface);
    CFDataRef data = CGDataProviderCopyData(CGImageGetDataProvider(image));
    assert(data && CFDataGetLength(data) == sizeof(expected) && !memcmp(CFDataGetBytePtr(data), expected, sizeof(expected)));
    CFRelease(data);
    CGImageRelease(image);
    surface = createSurface(0x52474241); // RGBA requires a different pixel interpretation and is intentionally unsupported.
    assert(surface && !LSBCopySurfaceImage(surface, &error) && error == ENOTSUP);
    CFRelease(surface);
    assert(!LSBCopySurfaceImage(NULL, &error) && error == EINVAL);
}

int main(void) {
    @autoreleasepool { testCoordinates(); testLayoutBounds(); testSurfaceSnapshot(); }
    puts("SimulatorDisplayTests passed");
    return 0;
}
