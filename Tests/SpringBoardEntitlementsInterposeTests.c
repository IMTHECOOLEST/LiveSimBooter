#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define UI_KEY CFSTR("com.apple.springboard-ui.client")

#ifdef LSB_SPRINGBOARD_ENTITLEMENTS_PROVIDER
bool LSBSpringBoardOriginalFixture(CFStringRef key) {
    errno = ERANGE;
    return key && CFEqual(key, CFSTR("original"));
}
#else
extern bool LSBSpringBoardOriginalFixture(CFStringRef key) __attribute__((weak_import));

int main(int argc, const char *argv[]) {
    assert(argc == 2);
    if (!strcmp(argv[1], "absent")) {
        // Use a unique original symbol: CoreFoundation can transitively load real BaseBoard on macOS.
        assert(!LSBSpringBoardOriginalFixture);
        bool (*wrapper)(CFStringRef) = dlsym(RTLD_DEFAULT, "LSBSpringBoardSelfTaskHasEntitlement");
        assert(wrapper);
        errno = EDOM;
        assert(!wrapper(UI_KEY) && errno == EDOM);
    } else {
        assert(LSBSpringBoardOriginalFixture);
        bool expected = !strcmp(argv[1], "enabled");
        assert(LSBSpringBoardOriginalFixture(CFSTR("original")) && errno == ERANGE);
        assert(!LSBSpringBoardOriginalFixture(CFSTR("other")) && errno == ERANGE);
        assert(LSBSpringBoardOriginalFixture(UI_KEY) == expected && errno == ERANGE);
    }
    printf("SpringBoard native interposition passed (%s)\n", argv[1]);
    return 0;
}
#endif
