#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

static unsigned calls;
static int failure;
static char observedName[33];
static int nameSysctl(int *mib, u_int count, void *old, size_t *oldLength, void *value, size_t length) {
    assert(count == 2 && mib[0] == CTL_KERN && mib[1] == KERN_PROCNAME);
    assert(!old && !oldLength && value && length > 1 && length <= sizeof(observedName));
    assert(((const char *)value)[length - 1] == 0);
    memcpy(observedName, value, length);
    ++calls;
    errno = failure;
    return failure ? -1 : 0;
}
#define sysctl nameSysctl
#include "../LiveProcess/ProcessDisplayName.c"
#undef sysctl

static void labelsAndBounds(void) {
    const char *labels[] = {"Metal.backboardd", "Metal.SpringBoard", "Metal.public", "/sbin/launchd", "/Applications/Contacts.app/Contacts"};
    for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); ++i) {
        struct { char before[8], argv0[128], after[32]; } storage;
        memset(&storage, 0xa5, sizeof(storage));
        strcpy(storage.argv0, "/private/var/containers/Bundle/Application/Test/LiveProcess.appex/LiveProcess");
        size_t capacity = strlen(storage.argv0) + 1;
        char original[sizeof(storage)];
        memcpy(original, &storage, sizeof(storage));
        errno = EDOM;
        assert(!LSBSetProcessDisplayName(labels[i], storage.argv0, capacity) && errno == EDOM);
        const char *expected = strrchr(labels[i], '/');
        expected = expected ? expected + 1 : labels[i];
        assert(!strcmp(storage.argv0, expected) && !strcmp(observedName, expected));
        for (size_t n = strlen(expected); n < capacity; ++n) assert(!storage.argv0[n]);
        assert(!memcmp(&storage, original, sizeof(storage.before)));
        size_t untouched = sizeof(storage.before) + capacity;
        assert(!memcmp((char *)&storage + untouched, original + untouched, sizeof(storage) - untouched));
    }
    char small[] = {'L', 'P', 0, 'X'};
    assert(!LSBSetProcessDisplayName("Metal.public", small, 3));
    assert(!strcmp(small, "Me") && small[3] == 'X' && !strcmp(observedName, "Metal.public"));
    char alias[64] = "/Applications/Contacts.app/Contacts";
    assert(!LSBSetProcessDisplayName(alias, alias, strlen(alias) + 1) && !strcmp(alias, "Contacts"));
    assert(!LSBSetProcessDisplayName("01234567890123456789012345678901234567", NULL, 0));
    assert(strlen(observedName) == 32 && !strcmp(observedName, "01234567890123456789012345678901"));
}

int main(void) {
    labelsAndBounds();
    char storage[] = "native argv";
    unsigned before = calls;
    assert(LSBSetProcessDisplayName(NULL, storage, sizeof(storage)) == EINVAL);
    assert(LSBSetProcessDisplayName("", storage, sizeof(storage)) == EINVAL);
    assert(LSBSetProcessDisplayName("/", storage, sizeof(storage)) == EINVAL);
    assert(LSBSetProcessDisplayName("valid", NULL, 1) == EINVAL);
    assert(calls == before && !strcmp(storage, "native argv"));
    failure = EPERM;
    errno = ERANGE;
    assert(LSBSetProcessDisplayName("Metal.public", storage, sizeof(storage)) == EPERM && errno == ERANGE);
    assert(!strcmp(storage, "Metal.publi"));
    puts("Process display name tests passed");
    return 0;
}
