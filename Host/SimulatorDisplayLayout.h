#pragma once

#include <CoreGraphics/CGGeometry.h>
#include <float.h>
#include <math.h>
#include <stdbool.h>

enum { LSBDisplayHomeButtonVisibleSize = 44, LSBDisplayHomeButtonHitSize = 48, LSBDisplayHomeButtonGap = 8 };
typedef struct { CGRect displayFrame, homeButtonFrame; } LSBDisplayLayout;

// The caller supplies its safe area in view coordinates. The guest and optional Home hit target form one centered group.
// Normal mode retains the 94% inset; full-width mode uses all safe space, but still shrinks to fit the group's height.
// Home stays below the guest in every orientation. Draw its 44-point circle inside the returned 48-point hit target.
// False clears both frames: invalid geometry or a space too small for the fixed Home target must never overlap or crop.
static inline bool LSBComputeSimulatorDisplayLayout(CGRect available, CGSize pixels, bool fullWidthEligible, bool fullWidthEnabled,
                                                    bool needsHomeButton, LSBDisplayLayout *output) {
    if (!output) return false;
    *output = (LSBDisplayLayout){CGRectZero, CGRectZero};
    if (!isfinite(available.origin.x) || !isfinite(available.origin.y) || !isfinite(available.size.width) ||
        !isfinite(available.size.height) || available.size.width <= 0 || available.size.height <= 0 ||
        !isfinite(pixels.width) || !isfinite(pixels.height) || pixels.width <= 0 || pixels.height <= 0) return false;
    CGFloat right = CGRectGetMaxX(available), bottom = CGRectGetMaxY(available);
    CGFloat aspect = pixels.width / pixels.height;
    if (!isfinite(right) || !isfinite(bottom) || right <= available.origin.x || bottom <= available.origin.y ||
        !isfinite(aspect) || aspect <= 0) return false;

    CGFloat fraction = fullWidthEligible && fullWidthEnabled ? 1 : 0.94;
    CGFloat maximumWidth = available.size.width * fraction, maximumHeight = available.size.height * fraction;
    CGFloat homeRow = needsHomeButton ? LSBDisplayHomeButtonHitSize + LSBDisplayHomeButtonGap : 0;
    if (needsHomeButton && (maximumWidth < LSBDisplayHomeButtonHitSize || maximumHeight <= homeRow)) return false;
    CGFloat heightBudget = maximumHeight - homeRow;
    CGFloat width = maximumWidth, height = width / aspect;
    if (height > heightBudget) { height = heightBudget; width = height * aspect; }
    if (!isfinite(width) || !isfinite(height) || width <= 0 || height <= 0) return false;
    CGFloat groupHeight = height + homeRow;
    CGRect display = CGRectMake(available.origin.x + (available.size.width - width) / 2,
                                available.origin.y + (available.size.height - groupHeight) / 2, width, height);
    CGRect home = CGRectZero;
    if (needsHomeButton) {
        home = CGRectMake(available.origin.x + (available.size.width - LSBDisplayHomeButtonHitSize) / 2,
                          CGRectGetMaxY(display) + LSBDisplayHomeButtonGap, LSBDisplayHomeButtonHitSize, LSBDisplayHomeButtonHitSize);
    }
    // Fractional safe-area coordinates can round the last endpoint or 8-point gap by one ulp. Allow only that rounding,
    // capped far below a display pixel; still reject unrepresentable coordinates and overflow at extreme finite inputs.
    CGFloat magnitude = fmax(1, fmax(fmax(fabs(available.origin.x), fabs(right)), fmax(fabs(available.origin.y), fabs(bottom))));
    CGFloat tolerance = fmin(1e-7, 8 * DBL_EPSILON * magnitude);
    if (!isfinite(display.origin.x) || !isfinite(display.origin.y) || available.origin.x - display.origin.x > tolerance ||
        available.origin.y - display.origin.y > tolerance || CGRectGetMaxX(display) - right > tolerance || CGRectGetMaxY(display) - bottom > tolerance ||
        CGRectGetMaxX(display) <= display.origin.x || CGRectGetMaxY(display) <= display.origin.y) return false;
    if (needsHomeButton && (!isfinite(home.origin.x) || !isfinite(home.origin.y) || available.origin.x - home.origin.x > tolerance ||
        CGRectGetMaxX(home) - right > tolerance || CGRectGetMaxY(home) - bottom > tolerance ||
        home.origin.y - CGRectGetMaxY(display) + tolerance < LSBDisplayHomeButtonGap)) return false;
    *output = (LSBDisplayLayout){display, home};
    return true;
}
