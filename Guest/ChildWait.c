#include "ChildWait.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static _Atomic(LSBChildStatus *) childStatus;
static _Atomic(int) childWakeFD = -1;
static _Atomic(int) configurationError = ENOTSUP;
_Static_assert(ATOMIC_POINTER_LOCK_FREE == 2, "Child wait lookup must remain signal-safe");

static void updateDiscard(const struct sigaction *action) {
    LSBChildStatus *ledger = atomic_load_explicit(&childStatus, memory_order_acquire);
    if (ledger) LSBChildStatusSetDiscard(ledger, action->sa_handler == SIG_IGN || (action->sa_flags & SA_NOCLDWAIT));
}

int LSBChildWaitConfigure(LSBChildStatus *ledger, int wakeFD) {
    int error = LSBChildStatusValidate(ledger);
    if (error) return error;
    int flags = fcntl(wakeFD, F_GETFL);
    if (flags < 0) return errno;
    if (!(flags & O_NONBLOCK) || (flags & O_ACCMODE) == O_WRONLY) return EINVAL;
    struct sigaction action;
    if (sigaction(SIGCHLD, NULL, &action)) return errno;
    atomic_store_explicit(&childWakeFD, wakeFD, memory_order_relaxed);
    atomic_store_explicit(&childStatus, ledger, memory_order_release);
    atomic_store_explicit(&configurationError, 0, memory_order_relaxed);
    updateDiscard(&action);
    return 0;
}

int LSBGuestChildWakeFD(void) { return atomic_load_explicit(&childWakeFD, memory_order_relaxed); }

#ifndef LSB_CHILD_WAIT_TESTING
__attribute__((constructor)) static void initializeChildWait(void) {
    const char *address = getenv(LSB_CHILD_STATUS_ENV), *descriptor = getenv(LSB_CHILD_WAKE_FD_ENV);
    int error = EINVAL;
    if (address && descriptor && *address && *descriptor) {
        char *end;
        errno = 0;
        unsigned long long pointer = strtoull(address, &end, 16);
        if (!errno && !*end && pointer && pointer <= UINTPTR_MAX) {
            errno = 0;
            long fd = strtol(descriptor, &end, 10);
            if (!errno && !*end && fd >= 0 && fd <= INT_MAX) error = LSBChildWaitConfigure((LSBChildStatus *)(uintptr_t)pointer, (int)fd);
        }
    }
    atomic_store_explicit(&configurationError, error, memory_order_relaxed);
}
#endif

static pid_t finishWait(pid_t child, int value, int *status, int savedErrno) {
    if (status) *status = value;
    errno = savedErrno;
    return child;
}

pid_t LSBWaitPID(pid_t selector, int *status, int options) {
    int savedErrno = errno;
    if (options & ~(WNOHANG | WUNTRACED | WCONTINUED)) { errno = EINVAL; return -1; }
    // The ledger reports terminal states only. Accept standard options without fabricating stopped or continued events.
    LSBChildStatus *ledger = atomic_load_explicit(&childStatus, memory_order_acquire);
    if (!ledger) { errno = atomic_load_explicit(&configurationError, memory_order_relaxed); return -1; }
    pid_t group = selector == 0 ? getpgrp() : 0;
    int wakeFD = LSBGuestChildWakeFD();
    for (;;) {
        pid_t child;
        int value;
        int error = LSBChildStatusTryWait(ledger, selector, group, &child, &value);
        if (!error) return finishWait(child, value, status, savedErrno);
        if (error != EAGAIN) { errno = error; return -1; }
        if (options & WNOHANG) { errno = savedErrno; return 0; }
        // Wake bytes are hints, never ownership of an exit. A bounded rescan handles another waiter draining the pipe.
        struct pollfd descriptor = {.fd = wakeFD, .events = POLLIN};
        int result = poll(&descriptor, 1, 100);
        if (result < 0) {
            error = errno;
            if (error == EINTR && !LSBChildStatusTryWait(ledger, selector, group, &child, &value))
                return finishWait(child, value, status, savedErrno);
            errno = error;
            return -1;
        }
        if (descriptor.revents & POLLNVAL) { errno = EBADF; return -1; }
        if (descriptor.revents & (POLLIN | POLLHUP)) {
            char hints[64];
            ssize_t count = read(wakeFD, hints, sizeof(hints));
            if (count > 0 || (count < 0 && (errno == EAGAIN || errno == EINTR))) continue;
            error = count == 0 ? EPIPE : errno;
            if (!LSBChildStatusTryWait(ledger, selector, group, &child, &value)) return finishWait(child, value, status, savedErrno);
            errno = error;
            return -1;
        }
        if (descriptor.revents & POLLERR) { errno = EIO; return -1; }
    }
}

pid_t LSBWait(int *status) { return LSBWaitPID(-1, status, 0); }

int LSBSigaction(int number, const struct sigaction *action, struct sigaction *oldAction) {
    // action and oldAction may alias; the kernel can overwrite the caller's input with the previous disposition.
    struct sigaction requested;
    if (number == SIGCHLD && action) requested = *action;
    int result = sigaction(number, action, oldAction);
    if (!result && number == SIGCHLD && action) updateDiscard(&requested);
    return result;
}

sig_t LSBSignal(int number, sig_t handler) {
    sig_t result = signal(number, handler);
    int savedErrno = errno;
    if (result != SIG_ERR && number == SIGCHLD) {
        struct sigaction action;
        if (!sigaction(number, NULL, &action)) updateDiscard(&action);
    }
    errno = savedErrno;
    return result;
}

#ifndef LSB_CHILD_WAIT_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBWaitPID, (const void *)&waitpid}, {(const void *)&LSBWait, (const void *)&wait},
    {(const void *)&LSBSigaction, (const void *)&sigaction}, {(const void *)&LSBSignal, (const void *)&signal}
};
#endif
