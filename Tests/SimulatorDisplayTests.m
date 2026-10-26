#define LSB_SIMULATOR_DISPLAY_CORE_ONLY 1
#include "../Host/SimulatorDisplayView.m"
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>

static void testCapture(CGImageRef image, const uint8_t expected[16]) {
    assert(LSBDisplayCaptureEnabled("1") && !LSBDisplayCaptureEnabled(NULL) && !LSBDisplayCaptureEnabled("0") &&
           !LSBDisplayCaptureEnabled("true") && !LSBDisplayCaptureEnabled("01"));
    char directory[] = "/tmp/LiveSimBooter-first-frame.XXXXXX";
    assert(mkdtemp(directory));
    NSString *folder = [NSString stringWithUTF8String:directory];
    NSString *path = [folder stringByAppendingPathComponent:@"LiveSimBooter-first-frame.png"];
    NSString *other = [folder stringByAppendingPathComponent:@"unrelated"];
    assert([@"unchanged" writeToFile:other atomically:YES encoding:NSUTF8StringEncoding error:NULL]);
    assert(!symlink(other.fileSystemRepresentation, path.fileSystemRepresentation));
    assert(!LSBCaptureDisplayImage(image, directory));
    assert([[NSString stringWithContentsOfFile:other encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"unchanged"]);
    struct stat status;
    assert(!lstat(path.fileSystemRepresentation, &status) && S_ISREG(status.st_mode) && (status.st_mode & 0777) == 0600);
    NSData *png = [NSData dataWithContentsOfFile:path];
    const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    assert(png.length > sizeof(signature) && !memcmp(png.bytes, signature, sizeof(signature)));
    CGImageSourceRef source = CGImageSourceCreateWithData((__bridge CFDataRef)png, NULL);
    CGImageRef decoded = source ? CGImageSourceCreateImageAtIndex(source, 0, NULL) : NULL;
    assert(decoded && CGImageGetWidth(decoded) == 2 && CGImageGetHeight(decoded) == 2);
    uint8_t pixels[16] = {0};
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels, 2, 2, 8, 8, color, kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    assert(context);
    CGContextDrawImage(context, CGRectMake(0, 0, 2, 2), decoded);
    assert(!memcmp(pixels, expected, 16)); // Producer was already reused and released before the diagnostic encoded the snapshot.
    CGContextRelease(context); CGColorSpaceRelease(color); CGImageRelease(decoded); CFRelease(source);
    assert(LSBCaptureDisplayImage(NULL, directory) == EINVAL && LSBCaptureDisplayImage(image, NULL) == EINVAL);
    assert([[NSData dataWithContentsOfFile:path] isEqualToData:png]);
    assert(!unlink(path.fileSystemRepresentation) && !mkdir(path.fileSystemRepresentation, 0700));
    assert(LSBCaptureDisplayImage(image, directory)); // Failed atomic publication leaves no hidden partial file.
    assert([[NSFileManager.defaultManager contentsOfDirectoryAtPath:folder error:NULL] count] == 2);
    assert(!rmdir(path.fileSystemRepresentation) && !unlink(other.fileSystemRepresentation) && !rmdir(directory));
    assert(LSBCaptureDisplayImage(image, directory) == ENOENT);
}

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

static void testBeginningEdges(void) {
    CGRect viewport = CGRectMake(10, 20, 300, 600);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(160, 20)) == LSBIndigoTouchEdgeTop);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(160, 32)) == LSBIndigoTouchEdgeTop);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(160, 32.01)) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(160, 608)) == LSBIndigoTouchEdgeBottom);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(160, 620)) == LSBIndigoTouchEdgeBottom);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(22, 320)) == LSBIndigoTouchEdgeLeft);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(298, 320)) == LSBIndigoTouchEdgeRight);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(160, 320)) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(10, 20)) == LSBIndigoTouchEdgeTop);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(310, 20)) == LSBIndigoTouchEdgeTop);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(10, 620)) == LSBIndigoTouchEdgeBottom);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(310, 620)) == LSBIndigoTouchEdgeBottom);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(15, 25)) == LSBIndigoTouchEdgeTop);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(14, 25)) == LSBIndigoTouchEdgeLeft);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(9, 20)) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(160, 19)) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(311, 320)) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(160, 621)) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(CGRectZero, CGPointZero) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(CGRectMake(0, 0, INFINITY, 100), CGPointZero) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(NAN, 20)) == LSBIndigoTouchEdgeNone);
    // Aspect-fit letterboxing never becomes an input edge, and the 5% cap prevents a tiny image becoming all edge.
    viewport = LSBDisplayViewport(CGRectMake(0, 0, 300, 200), CGSizeMake(100, 200));
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(99, 100)) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(105, 100)) == LSBIndigoTouchEdgeLeft);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(105.01, 100)) == LSBIndigoTouchEdgeNone);
    viewport = CGRectMake(0, 0, 10, 20);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(5, 0.5)) == LSBIndigoTouchEdgeTop);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(5, 0.51)) == LSBIndigoTouchEdgeNone);
    assert(LSBDisplayBeginningEdge(viewport, CGPointMake(5, 10)) == LSBIndigoTouchEdgeNone);
}

static void testSnapshotCapture(void) {
    const uint8_t expected[] = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
    uint8_t pixels[16];
    memcpy(pixels, expected, sizeof(pixels));
    CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels, 2, 2, 8, 8, color, kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    assert(context);
    CGImageRef image = CGBitmapContextCreateImage(context);
    assert(image);
    CGContextRelease(context); CGColorSpaceRelease(color);
    memset(pixels, 0, sizeof(pixels));
    testCapture(image, expected);
    CGImageRelease(image);
}

int main(void) {
    @autoreleasepool { testCoordinates(); testBeginningEdges(); testLayoutBounds(); testSnapshotCapture(); }
    puts("SimulatorDisplayTests: coordinate mapping, edge origins, and diagnostic PNG publication passed");
    return 0;
}
