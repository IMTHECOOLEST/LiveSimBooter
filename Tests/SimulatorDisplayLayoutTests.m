#include "../Host/SimulatorDisplayLayout.h"
#include <assert.h>
#include <float.h>
#include <stdio.h>

static bool near(CGFloat left, CGFloat right) { return fabs(left - right) <= 1e-8 * fmax(1, fmax(fabs(left), fabs(right))); }

static void checkLayout(CGRect available, CGSize pixels, bool eligible, bool enabled, bool home) {
    LSBDisplayLayout layout;
    assert(LSBComputeSimulatorDisplayLayout(available, pixels, eligible, enabled, home, &layout));
    CGRect display = layout.displayFrame, button = layout.homeButtonFrame;
    assert(display.size.width > 0 && display.size.height > 0);
    assert(near(display.size.width / display.size.height, pixels.width / pixels.height));
    CGRect roundedBounds = CGRectInset(available, -1e-7, -1e-7);
    assert(CGRectContainsRect(roundedBounds, display));
    assert(near(CGRectGetMidX(display), CGRectGetMidX(available)));
    CGFloat fraction = eligible && enabled ? 1 : 0.94;
    CGFloat row = home ? 56 : 0;
    assert(display.size.width <= available.size.width * fraction + 1e-8);
    assert(display.size.height + row <= available.size.height * fraction + 1e-8);
    // At least one dimension fills the usable budget: fitting must not needlessly sacrifice width.
    assert(near(display.size.width, available.size.width * fraction) || near(display.size.height + row, available.size.height * fraction));
    CGFloat groupBottom = CGRectGetMaxY(display);
    if (home) {
        assert(button.size.width == 48 && button.size.height == 48 && CGRectContainsRect(roundedBounds, button));
        assert(near(CGRectGetMidX(button), CGRectGetMidX(available)));
        assert(near(button.origin.y - CGRectGetMaxY(display), 8) && !CGRectIntersectsRect(display, button));
        assert(CGRectContainsRect(button, CGRectInset(button, 2, 2)) && CGRectInset(button, 2, 2).size.width == 44);
        groupBottom = CGRectGetMaxY(button);
    } else assert(CGRectEqualToRect(button, CGRectZero));
    assert(near(display.origin.y - available.origin.y, CGRectGetMaxY(available) - groupBottom));
}

static void deviceAndOrientationTests(void) {
    const struct { CGSize pixels; bool home; } devices[] = {
        {{750, 1334}, true}, {{1242, 2208}, true}, {{750, 1334}, true}, {{828, 1792}, false},
        {{1290, 2796}, false}, {{1620, 2160}, true}, {{1640, 2360}, false}
    };
    const CGRect available[] = {
        {{0, 59}, {430, 790}}, {{59, 0}, {780, 350}}, {{20, 24}, {984, 1300}}, {{24, 20}, {1300, 900}},
        {{-50, -30}, {400, 900}}, {{12.25, 34.75}, {393.333333333, 761.666666667}},
        {{0, 57.333333333333336}, {932.6666666666666, 1013}},
        {{9.666666666666666, 47.666666666666664}, {100.66666666666667, 289.3333333333333}}
    };
    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); ++i) {
        for (unsigned j = 0; j < sizeof(available) / sizeof(available[0]); ++j) {
            for (unsigned flags = 0; flags < 4; ++flags) {
                checkLayout(available[j], devices[i].pixels, flags & 1, flags & 2, devices[i].home);
                // A guest surface can rotate independently of the native phone's orientation.
                checkLayout(available[j], CGSizeMake(devices[i].pixels.height, devices[i].pixels.width),
                            flags & 1, flags & 2, devices[i].home);
            }
        }
    }
}

static void expectedGeometryTests(void) {
    LSBDisplayLayout full, fit, ineligible;
    CGRect tall = CGRectMake(20, 50, 400, 900);
    CGSize phone = CGSizeMake(750, 1334);
    assert(LSBComputeSimulatorDisplayLayout(tall, phone, true, true, true, &full));
    assert(full.displayFrame.origin.x == 20 && full.displayFrame.size.width == 400);
    assert(near(full.displayFrame.size.height, 400.0 * 1334 / 750));
    assert(near(full.displayFrame.origin.y, 50 + (900 - 400.0 * 1334 / 750 - 56) / 2));
    assert(LSBComputeSimulatorDisplayLayout(tall, phone, true, false, true, &fit));
    assert(near(fit.displayFrame.size.width, 376));
    assert(LSBComputeSimulatorDisplayLayout(tall, phone, false, true, true, &ineligible));
    assert(CGRectEqualToRect(fit.displayFrame, ineligible.displayFrame) && CGRectEqualToRect(fit.homeButtonFrame, ineligible.homeButtonFrame));

    CGRect shortArea = CGRectMake(59, 0, 780, 350);
    assert(LSBComputeSimulatorDisplayLayout(shortArea, phone, true, true, true, &full));
    assert(full.displayFrame.origin.y == 0 && full.displayFrame.size.height == 294);
    assert(near(full.displayFrame.size.width, 294.0 * 750 / 1334));
    assert(full.homeButtonFrame.origin.y == 302 && CGRectGetMaxY(full.homeButtonFrame) == 350);
    assert(LSBComputeSimulatorDisplayLayout(CGRectMake(0, 0, 100, 200), CGSizeMake(1, 2), false, false, false, &fit));
    assert(CGRectEqualToRect(fit.displayFrame, CGRectMake(3, 6, 94, 188))); // Original 94% fit, with no Home row.
}

static void assertInvalid(CGRect available, CGSize pixels, bool home) {
    LSBDisplayLayout output = {CGRectMake(1, 2, 3, 4), CGRectMake(5, 6, 7, 8)};
    assert(!LSBComputeSimulatorDisplayLayout(available, pixels, true, true, home, &output));
    assert(CGRectEqualToRect(output.displayFrame, CGRectZero) && CGRectEqualToRect(output.homeButtonFrame, CGRectZero));
}

static void invalidAndExtremeTests(void) {
    CGRect available = CGRectMake(0, 0, 430, 790);
    CGSize pixels = CGSizeMake(750, 1334);
    assert(!LSBComputeSimulatorDisplayLayout(available, pixels, true, true, true, NULL));
    const CGRect invalid[] = {
        {{0, 0}, {0, 100}}, {{0, 0}, {100, 0}}, {{0, 0}, {-1, 100}}, {{0, 0}, {100, -1}},
        {{NAN, 0}, {100, 100}}, {{0, NAN}, {100, 100}}, {{INFINITY, 0}, {100, 100}}, {{0, 0}, {INFINITY, 100}},
        {{0, 0}, {100, NAN}}, {{DBL_MAX, 0}, {DBL_MAX, 100}}, {{0, DBL_MAX}, {100, DBL_MAX}}, {{1e30, 0}, {1, 100}}
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) assertInvalid(invalid[i], pixels, false);
    const CGSize invalidPixels[] = {{0, 1}, {1, 0}, {-1, 1}, {1, -1}, {NAN, 1}, {1, INFINITY}, {DBL_MAX, DBL_MIN}, {DBL_MIN, DBL_MAX}};
    for (unsigned i = 0; i < sizeof(invalidPixels) / sizeof(invalidPixels[0]); ++i) assertInvalid(available, invalidPixels[i], false);
    assertInvalid(CGRectMake(0, 0, 47, 900), pixels, true);
    assertInvalid(CGRectMake(0, 0, 400, 56), pixels, true);
    checkLayout(CGRectMake(0, 0, 48, 57), pixels, true, true, true); // Minimum button width and a nonzero guest image.
    checkLayout(CGRectMake(0, 0, 0.01, 0.02), CGSizeMake(1, 2), true, true, false);
    checkLayout(CGRectMake(-1e6, 1e6, 1e6, 2e6), pixels, true, true, false);
}

int main(void) {
    deviceAndOrientationTests();
    expectedGeometryTests();
    invalidAndExtremeTests();
    puts("SimulatorDisplayLayoutTests passed: seven profiles, both orientations, full-width fitting, Home separation, and invalid bounds");
    return 0;
}
