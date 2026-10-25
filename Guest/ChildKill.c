#include "ChildKill.h"
#include "ProcessIdentity.h"
#include "../ProcessSupport/SpawnTransport.h"

#include <errno.h>
#include <signal.h>
#include <unistd.h>

int LSBKill(pid_t pid, int signalNumber) {
    if (signalNumber < 0 || signalNumber >= NSIG) { errno = EINVAL; return -1; }
    if (pid <= 0) { errno = ENOTSUP; return -1; }
    // References from this image retain the original implementation, as with the signal and sigaction interposers.
    if (pid == LSBGuestPID()) return kill(getpid(), signalNumber);
    int savedErrno = errno;
    int error = LSBTransportKill(pid, signalNumber);
    if (error) { errno = error; return -1; }
    errno = savedErrno;
    return 0;
}

#ifndef LSB_CHILD_KILL_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBKill, (const void *)&kill}
};
#endif
