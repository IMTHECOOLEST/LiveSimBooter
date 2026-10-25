#include "../ProcessSupport/SpawnTransport.h"
#include "../ProcessSupport/SingleThreadGuard.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

static const char *kernelRelease;
static int releaseError, guardError, blockError, restoreInterrupts;
static bool missingQuery, interruptCapture;
static unsigned queries, lookups, maskChanges, guards, blocks, restores;
static mode_t queryResult;
static volatile sig_atomic_t deliveredSignals, maskInHandler;

static int testSysctl(int *name, u_int count, void *output, size_t *length, void *newValue, size_t newLength) {
    assert(count == 2 && name[0] == CTL_KERN && name[1] == KERN_OSRELEASE && !newValue && !newLength);
    if (releaseError) { errno = releaseError; return -1; }
    size_t bytes = strlen(kernelRelease) + 1;
    assert(*length >= bytes);
    memcpy(output, kernelRelease, bytes);
    *length = bytes;
    return 0;
}

static mode_t testGetumask(void) { ++queries; return queryResult; }
static void *testDlsym(void *scope, const char *symbol) {
    assert(scope == RTLD_DEFAULT && !strcmp(symbol, "getumask"));
    ++lookups;
    return missingQuery ? NULL : (void *)&testGetumask;
}
static int testSingleThread(void) { ++guards; return guardError ? guardError : LSBSingleThreadGuard(); }
static int testSigprocmask(int how, const sigset_t *set, sigset_t *previous) {
    if (how == SIG_BLOCK) {
        ++blocks;
        if (blockError) { errno = blockError; return -1; }
    } else {
        assert(how == SIG_SETMASK);
        ++restores;
        if (restoreInterrupts) { --restoreInterrupts; errno = EINTR; return -1; }
    }
    return sigprocmask(how, set, previous);
}
static mode_t testUmask(mode_t mask) {
    ++maskChanges;
    mode_t previous = umask(mask);
    if (!mask && interruptCapture) {
        assert(!raise(SIGUSR1));
        assert(!deliveredSignals);
    }
    return previous;
}

#define sysctl testSysctl
#define dlsym testDlsym
#define LSBRequireSingleThread testSingleThread
#define sigprocmask testSigprocmask
#define umask testUmask
#include "../Guest/CreationMask.c"
#undef sysctl
#undef dlsym
#undef LSBRequireSingleThread
#undef sigprocmask
#undef umask

static void signalHandler(int number) {
    int savedErrno = errno;
    assert(number == SIGUSR1);
    mode_t current = umask(0);
    umask(current);
    maskInHandler = current;
    ++deliveredSignals;
    errno = savedErrno;
}

static void reset(void) {
    kernelRelease = "25.1.0";
    releaseError = guardError = blockError = restoreInterrupts = 0;
    missingQuery = interruptCapture = false;
    queries = lookups = maskChanges = guards = blocks = restores = 0;
    queryResult = 077;
    deliveredSignals = 0;
    maskInHandler = -1;
}

static void assertMaskUnchanged(mode_t expected) {
    mode_t current = umask(expected);
    assert(current == expected);
}

static void testOldKernel(void) {
    reset();
    uint32_t result = UINT32_MAX;
    errno = EDOM;
    assert(!LSBReadCreationMask(&result) && result == 027 && errno == EDOM);
    assert(!queries && !lookups && guards == 1 && blocks == 1 && restores == 1 && maskChanges == 2);
    assertMaskUnchanged(027);

    reset();
    struct sigaction action = {.sa_handler = signalHandler}, previous;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1, &action, &previous));
    interruptCapture = true;
    restoreInterrupts = 1;
    errno = EDOM;
    assert(!LSBReadCreationMask(&result) && result == 027 && errno == EDOM);
    assert(deliveredSignals == 1 && maskInHandler == 027 && restores == 2);
    assert(!sigaction(SIGUSR1, &previous, NULL));
    assertMaskUnchanged(027);
}

static void testFailureRestoresState(void) {
    for (unsigned variant = 0; variant < 3; ++variant) {
        reset();
        sigset_t before, after;
        assert(!sigprocmask(SIG_SETMASK, NULL, &before));
        uint32_t result = UINT32_MAX;
        if (variant == 0) guardError = EBUSY;
        if (variant == 1) guardError = EIO;
        if (variant == 2) blockError = EPERM;
        errno = EDOM;
        assert(LSBReadCreationMask(&result) == (blockError ? blockError : guardError) && result == UINT32_MAX && errno == EDOM);
        assert(!sigprocmask(SIG_SETMASK, NULL, &after) && !memcmp(&before, &after, sizeof(before)));
        assert(!maskChanges && !queries && restores == (variant == 2 ? 0u : 1u));
        assertMaskUnchanged(027);
    }
    errno = EDOM;
    assert(LSBReadCreationMask(NULL) == EINVAL && errno == EDOM);
}

static void *blockedWorker(void *context) {
    char byte;
    assert(read(*(int *)context, &byte, 1) == 1);
    return NULL;
}

static void testRealThreadGuard(void) {
    reset();
    int gate[2];
    assert(!pipe(gate));
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, blockedWorker, &gate[0]));
    uint32_t result = UINT32_MAX;
    assert(LSBReadCreationMask(&result) == EBUSY && result == UINT32_MAX && !maskChanges && restores == 1);
    assert(write(gate[1], "x", 1) == 1 && !pthread_join(worker, NULL));
    close(gate[0]); close(gate[1]);
    int guard = EBUSY;
    for (unsigned attempt = 0; attempt < 100 && guard == EBUSY; ++attempt) { usleep(1000); guard = LSBSingleThreadGuard(); }
    assert(!guard);
}

static void testKernelSelection(void) {
    const char *unknown[] = {"", "26", "x26.0", "-26.0", "9999999999999999999999999.0", "24.9.0", "25.1.0"};
    for (unsigned i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        reset();
        kernelRelease = unknown[i];
        uint32_t result;
        assert(!LSBReadCreationMask(&result) && result == 027 && !queries && !lookups && maskChanges == 2);
    }
    reset();
    releaseError = ENOTSUP;
    uint32_t result;
    assert(!LSBReadCreationMask(&result) && result == 027 && !queries && !lookups);
    reset();
    kernelRelease = "26.0.0";
    assert(!LSBReadCreationMask(&result) && result == 077 && queries == 1 && lookups == 1 && !guards && !maskChanges && !blocks);
    assertMaskUnchanged(027);
    reset();
    kernelRelease = "26.1.0";
    missingQuery = true;
    assert(!LSBReadCreationMask(&result) && result == 027 && !queries && lookups == 1 && maskChanges == 2);
    reset();
    kernelRelease = "26.1.0";
    queryResult = (mode_t)-1;
    result = UINT32_MAX;
    errno = EDOM;
    assert(LSBReadCreationMask(&result) == EPROTO && result == UINT32_MAX && errno == EDOM && !maskChanges);
}

int main(void) {
    alarm(10);
    mode_t original = umask(027);
    testOldKernel();
    testFailureRestoresState();
    testRealThreadGuard();
    testKernelSelection();
    umask(original);
    puts("CreationMask: kernel gating, old-kernel mask restoration, blocked signal delivery, real thread rejection, and errno preservation passed");
    return 0;
}
