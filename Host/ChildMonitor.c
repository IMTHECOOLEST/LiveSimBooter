#include "ChildMonitor.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/event.h>
#include <unistd.h>

// Exported by libSystem on iOS; the public iPhoneOS SDK omits libproc.h.
extern int proc_signal_with_audittoken(audit_token_t *audittoken, int signal);

int LSBChildMonitorCreate(pid_t pid, int *descriptor) {
    if (pid <= 0 || !descriptor) return EINVAL;
    *descriptor = -1;
    int queue = kqueue();
    if (queue < 0) return errno;
    int error = 0;
    if (fcntl(queue, F_SETFD, FD_CLOEXEC) < 0) error = errno;
    struct kevent change, receipt;
    EV_SET(&change, (uintptr_t)pid, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_ONESHOT | EV_RECEIPT, NOTE_EXIT | NOTE_EXITSTATUS, 0, NULL);
    if (!error) {
        int count;
        do { count = kevent(queue, &change, 1, &receipt, 1, NULL); } while (count < 0 && errno == EINTR);
        if (count < 0) error = errno;
        else if (count != 1 || !(receipt.flags & EV_ERROR)) error = EPROTO;
        else if (receipt.data) error = (int)receipt.data;
    }
    if (error) { close(queue); return error; }
    *descriptor = queue;
    return 0;
}

int LSBChildMonitorRead(int descriptor, pid_t pid, int *waitStatus) {
    if (descriptor < 0 || pid <= 0 || !waitStatus) return EINVAL;
    struct kevent event;
    struct timespec immediate = {0, 0};
    int count;
    do { count = kevent(descriptor, NULL, 0, &event, 1, &immediate); } while (count < 0 && errno == EINTR);
    if (count < 0) return errno;
    if (!count) return EAGAIN;
    if (event.flags & EV_ERROR) return event.data ? (int)event.data : EPROTO;
    if (event.filter != EVFILT_PROC || event.ident != (uintptr_t)pid ||
        (event.fflags & (NOTE_EXIT | NOTE_EXITSTATUS)) != (NOTE_EXIT | NOTE_EXITSTATUS)) return EPROTO;
    *waitStatus = (int)event.data;
    return 0;
}

int LSBChildSignal(const audit_token_t *audit, int signal) {
    if (!audit) return EINVAL;
    audit_token_t identity = *audit;
    // libproc converts its syscall's -1/errno result into a positive errno return value.
    return proc_signal_with_audittoken(&identity, signal);
}
