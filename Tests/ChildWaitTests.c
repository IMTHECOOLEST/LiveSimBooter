#include "../Guest/ChildWait.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static LSBChildStatus ledger;
static int wakeFDs[2];
static volatile sig_atomic_t handlerPID, handlerStatus, handlerError;

static LSBChildTicket running(pid_t pid, pid_t group) {
    LSBChildTicket ticket;
    assert(!LSBChildStatusReserve(&ledger, &ticket));
    assert(!LSBChildStatusPublishRunning(&ledger, ticket, pid, group));
    return ticket;
}

static void reset(void) {
    struct sigaction action = {.sa_handler = SIG_DFL};
    sigemptyset(&action.sa_mask);
    assert(!LSBSigaction(SIGCHLD, &action, NULL));
    LSBChildStatusInitialize(&ledger);
    assert(!LSBChildWaitConfigure(&ledger, wakeFDs[0]));
    char buffer[64];
    while (read(wakeFDs[0], buffer, sizeof(buffer)) > 0) {}
}

static void testNoHangAndSelectors(void) {
    reset();
    LSBChildTicket first = running(100, getpgrp()), second = running(101, 222);
    int status = 0x1234;
    errno = EDOM;
    assert(LSBWaitPID(100, &status, WNOHANG) == 0 && status == 0x1234 && errno == EDOM);
    assert(LSBWaitPID(999, &status, WNOHANG) == -1 && errno == ECHILD && status == 0x1234);
    assert(LSBWaitPID(100, &status, WUNTRACED) == -1 && errno == ENOTSUP);
    assert(LSBWaitPID(100, &status, WCONTINUED) == -1 && errno == ENOTSUP);
    assert(LSBWaitPID(100, &status, 0x40000000) == -1 && errno == EINVAL);
    assert(!LSBChildStatusPublishExit(&ledger, first, 0));
    errno = ERANGE;
    assert(LSBWaitPID(0, &status, WNOHANG) == 100 && status == 0 && errno == ERANGE);
    assert(LSBWaitPID(100, &status, WNOHANG) == -1 && errno == ECHILD);
    assert(!LSBChildStatusPublishExit(&ledger, second, SIGTERM));
    assert(LSBWaitPID(-222, &status, WNOHANG) == 101 && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(LSBWait(NULL) == -1 && errno == ECHILD);
}

typedef struct { LSBChildTicket ticket; int status, wake; } Completion;
static void *completeLater(void *pointer) {
    Completion *completion = pointer;
    usleep(30000);
    assert(!LSBChildStatusPublishExit(&ledger, completion->ticket, completion->status));
    if (completion->wake) assert(write(wakeFDs[1], "x", 1) == 1);
    return NULL;
}

static void testBlockingWait(void) {
    for (unsigned wake = 0; wake < 2; ++wake) {
        reset();
        Completion completion = {running(201, getpgrp()), 37 << 8, (int)wake};
        pthread_t worker;
        assert(!pthread_create(&worker, NULL, completeLater, &completion));
        int status;
        assert(LSBWaitPID(-1, &status, 0) == 201 && WIFEXITED(status) && WEXITSTATUS(status) == 37);
        assert(!pthread_join(worker, NULL));
    }
}

static void reapInHandler(int number) {
    (void)number;
    int saved = errno, status = -1;
    handlerPID = LSBWaitPID(-1, &status, WNOHANG);
    handlerStatus = status;
    handlerError = errno;
    errno = saved;
}

static void testSignalHandlerReapAndDiscard(void) {
    reset();
    struct sigaction action = {.sa_handler = reapInHandler};
    sigemptyset(&action.sa_mask);
    assert(!LSBSigaction(SIGCHLD, &action, NULL));
    LSBChildTicket ticket = running(301, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 42 << 8));
    errno = ENOTTY;
    assert(!raise(SIGCHLD));
    assert(handlerPID == 301 && WEXITSTATUS(handlerStatus) == 42 && handlerError == ENOTTY && errno == ENOTTY);
    assert(LSBWaitPID(301, NULL, WNOHANG) == -1 && errno == ECHILD);

    ticket = running(302, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    action.sa_handler = SIG_IGN;
    // Native sigaction permits its input and output to alias. The shim must publish the requested policy, not the old one.
    assert(!LSBSigaction(SIGCHLD, &action, &action));
    assert(LSBWaitPID(302, NULL, WNOHANG) == -1 && errno == ECHILD);
    ticket = running(303, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    assert(LSBWaitPID(303, NULL, WNOHANG) == -1 && errno == ECHILD);
    assert(LSBSignal(SIGCHLD, SIG_DFL) != SIG_ERR);
    ticket = running(304, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 9 << 8));
    int status;
    assert(LSBWaitPID(304, &status, WNOHANG) == 304 && WEXITSTATUS(status) == 9);
    action = (struct sigaction){.sa_handler = SIG_DFL, .sa_flags = SA_NOCLDWAIT};
    sigemptyset(&action.sa_mask);
    assert(!LSBSigaction(SIGCHLD, &action, NULL));
    ticket = running(305, getpgrp());
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    assert(LSBWaitPID(305, NULL, WNOHANG) == -1 && errno == ECHILD);
}

static void interruptHandler(int number) { (void)number; }
static void *interruptLater(void *pointer) {
    usleep(30000);
    assert(!pthread_kill(*(pthread_t *)pointer, SIGUSR1));
    return NULL;
}

static void testInterruptedWait(void) {
    reset();
    LSBChildTicket ticket = running(401, getpgrp());
    struct sigaction action = {.sa_handler = interruptHandler};
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGUSR1, &action, NULL));
    pthread_t mainThread = pthread_self(), worker;
    assert(!pthread_create(&worker, NULL, interruptLater, &mainThread));
    int status = 0x2222;
    assert(LSBWaitPID(401, &status, 0) == -1 && errno == EINTR && status == 0x2222);
    assert(!pthread_join(worker, NULL));
    assert(!LSBChildStatusAbandon(&ledger, ticket));
}

int main(void) {
    alarm(15);
    assert(!pipe(wakeFDs) && !fcntl(wakeFDs[0], F_SETFL, O_NONBLOCK));
    testNoHangAndSelectors();
    testBlockingWait();
    testSignalHandlerReapAndDiscard();
    testInterruptedWait();
    close(wakeFDs[0]); close(wakeFDs[1]);
    puts("ChildWait: WNOHANG, PID/groups, exit/signal status, blocking/lost wakes, signal-handler reaping, discard policy and EINTR passed");
    return 0;
}
