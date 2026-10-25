#include "CreationMask.h"
#include "../ProcessSupport/SpawnTransport.h"

#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

static bool kernelHasGetumask(void) {
    // Simulator libraries can export newer syscall stubs than the phone's kernel implements.
    // Query the actual kernel release, not the simulator SDK/runtime or dlsym symbol availability.
    int mib[] = {CTL_KERN, KERN_OSRELEASE};
    char release[64];
    size_t length = sizeof(release);
    if (sysctl(mib, 2, release, &length, NULL, 0) || !length || length > sizeof(release) || release[length - 1]) return false;
    unsigned major = 0;
    size_t index = 0;
    while (index < length && release[index] >= '0' && release[index] <= '9') {
        unsigned digit = (unsigned)(release[index++] - '0');
        if (major > (UINT_MAX - digit) / 10) return false;
        major = major * 10 + digit;
    }
    return index && index < length && release[index] == '.' && major >= 26;
}

static int captureMask(uint32_t *mask) {
    if (!mask) return EINVAL;
    if (kernelHasGetumask()) {
        mode_t (*queryMask)(void) = (mode_t (*)(void))dlsym(RTLD_DEFAULT, "getumask");
        if (queryMask) {
            mode_t value = queryMask();
            if (value & ~0777u) return EPROTO;
            *mask = value;
            return 0;
        }
    }
    // No other thread or catchable signal handler may create a file while the process mask is temporarily zero.
    sigset_t allSignals, savedSignals;
    if (sigfillset(&allSignals) || sigprocmask(SIG_BLOCK, &allSignals, &savedSignals)) return errno;
    int error = LSBRequireSingleThread();
    mode_t value = 0;
    if (!error) {
        value = umask(0);
        umask(value);
    }
    int restored;
    do { restored = sigprocmask(SIG_SETMASK, &savedSignals, NULL); } while (restored && errno == EINTR);
    if (restored) return errno;
    if (!error) *mask = value;
    return error;
}

int LSBReadCreationMask(uint32_t *mask) {
    int savedErrno = errno;
    int error = captureMask(mask);
    errno = savedErrno;
    return error;
}
