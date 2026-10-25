#include "../Host/ChildMonitor.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static void observeGrandchild(int signalNumber, int exitCode) {
    int report[2], release[2];
    assert(!pipe(report) && !pipe(release));
    pid_t parent = fork();
    assert(parent >= 0);
    if (!parent) {
        close(report[0]); close(release[1]);
        pid_t child = fork();
        if (child < 0) _exit(90);
        if (!child) {
            close(report[1]);
            char byte;
            if (read(release[0], &byte, 1) != 1) _exit(91);
            if (signalNumber) { signal(signalNumber, SIG_DFL); raise(signalNumber); }
            _exit(exitCode);
        }
        close(release[0]);
        if (write(report[1], &child, sizeof(child)) != sizeof(child)) _exit(92);
        int status;
        if (waitpid(child, &status, 0) != child || write(report[1], &status, sizeof(status)) != sizeof(status)) _exit(93);
        _exit(0);
    }
    close(report[1]); close(release[0]);
    pid_t child;
    assert(read(report[0], &child, sizeof(child)) == sizeof(child));
    int monitor = -1, observed = -1;
    assert(LSBChildMonitorCreate(child, &monitor) == 0);
    assert(LSBChildMonitorRead(monitor, child, &observed) == EAGAIN && observed == -1);
    assert(write(release[1], "x", 1) == 1);
    close(release[1]);
    struct pollfd ready = {.fd = monitor, .events = POLLIN};
    assert(poll(&ready, 1, 5000) == 1);
    assert(LSBChildMonitorRead(monitor, child, &observed) == 0);
    int actual;
    assert(read(report[0], &actual, sizeof(actual)) == sizeof(actual));
    assert(observed == actual);
    if (signalNumber) assert(WIFSIGNALED(observed) && WTERMSIG(observed) == signalNumber);
    else assert(WIFEXITED(observed) && WEXITSTATUS(observed) == exitCode);
    close(monitor); close(report[0]);
    assert(waitpid(parent, &actual, 0) == parent && WIFEXITED(actual) && WEXITSTATUS(actual) == 0);
}

int main(void) {
    audit_token_t missing = {{0}};
    missing.val[5] = INT_MAX;
    // The libproc API returns a positive errno; treating only negative values as failures loses sandbox denials.
    assert(LSBChildSignal(&missing, SIGCHLD) == ESRCH);
    observeGrandchild(0, 0);
    observeGrandchild(0, 37);
    observeGrandchild(SIGTERM, 0);
    puts("Child monitor: real non-child exit 0, exit 37, and SIGTERM statuses passed");
    return 0;
}
