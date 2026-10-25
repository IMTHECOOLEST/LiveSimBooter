#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

static inline CGRect LSBDisplayViewport(CGRect bounds, CGSize pixels) {
    if (!isfinite(bounds.origin.x) || !isfinite(bounds.origin.y) || !isfinite(bounds.size.width) || !isfinite(bounds.size.height) ||
        !isfinite(pixels.width) || !isfinite(pixels.height) || bounds.size.width <= 0 || bounds.size.height <= 0 ||
        pixels.width <= 0 || pixels.height <= 0) return CGRectZero;
    CGFloat scale = fmin(bounds.size.width / pixels.width, bounds.size.height / pixels.height);
    CGSize size = CGSizeMake(pixels.width * scale, pixels.height * scale);
    CGRect result = CGRectMake(bounds.origin.x + (bounds.size.width - size.width) / 2,
                               bounds.origin.y + (bounds.size.height - size.height) / 2, size.width, size.height);
    if (!isfinite(result.origin.x) || !isfinite(result.origin.y) || !isfinite(size.width) || !isfinite(size.height) ||
        size.width <= 0 || size.height <= 0) return CGRectZero;
    return result;
}

// A begin must be inside the image. Later phases retain the contact and clamp to the nearest image edge.
static inline bool LSBDisplayNormalize(CGRect viewport, CGPoint point, bool beginning, CGPoint *normalized) {
    if (!normalized || !isfinite(point.x) || !isfinite(point.y) || !isfinite(viewport.origin.x) || !isfinite(viewport.origin.y) ||
        !isfinite(viewport.size.width) || !isfinite(viewport.size.height) || viewport.size.width <= 0 || viewport.size.height <= 0) return false;
    double x = (point.x - viewport.origin.x) / viewport.size.width, y = (point.y - viewport.origin.y) / viewport.size.height;
    if (!isfinite(x) || !isfinite(y) || (beginning && (x < 0 || x > 1 || y < 0 || y > 1))) return false;
    *normalized = CGPointMake(fmax(0, fmin(1, x)), fmax(0, fmin(1, y)));
    return true;
}

static inline int LSBDisplayBGRALayout(size_t width, size_t height, size_t stride, size_t allocation, size_t *rowBytes, size_t *copyBytes) {
    if (!width || !height || !rowBytes || !copyBytes) return EINVAL;
    if (width > SIZE_MAX / 4) return EOVERFLOW;
    size_t row = width * 4;
    if (stride < row) return EINVAL;
    if (height > SIZE_MAX / row || height - 1 > (SIZE_MAX - row) / stride) return EOVERFLOW;
    if ((height - 1) * stride + row > allocation) return EINVAL;
    *rowBytes = row;
    *copyBytes = height * row;
    return 0;
}
