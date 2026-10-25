#include "../ProcessSupport/ChildStatus.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

static LSBChildStatus ledger;

static LSBChildTicket running(pid_t pid, pid_t group) {
    LSBChildTicket ticket;
    assert(!LSBChildStatusReserve(&ledger, &ticket));
    assert(!LSBChildStatusPublishRunning(&ledger, ticket, pid, group));
    return ticket;
}

static void testSelectionAndReuse(void) {
    LSBChildStatusInitialize(&ledger);
    assert(!LSBChildStatusValidate(&ledger));
    pid_t pid = 99;
    int status = 88;
    assert(LSBChildStatusTryWait(&ledger, -1, 0, &pid, &status) == ECHILD && pid == 99 && status == 88);
    LSBChildTicket first = running(10, 7), second = running(11, 8);
    assert(LSBChildStatusTryWait(&ledger, 10, 0, &pid, &status) == EAGAIN && pid == 99 && status == 88);
    assert(LSBChildStatusTryWait(&ledger, 12, 0, &pid, &status) == ECHILD);
    assert(!LSBChildStatusPublishExit(&ledger, first, 37 << 8));
    assert(!LSBChildStatusPublishExit(&ledger, second, SIGTERM));
    assert(LSBChildStatusTryWait(&ledger, 0, 9, &pid, &status) == ECHILD);
    assert(!LSBChildStatusTryWait(&ledger, -8, 0, &pid, &status) && pid == 11 && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    assert(!LSBChildStatusTryWait(&ledger, 0, 7, &pid, &status) && pid == 10 && WIFEXITED(status) && WEXITSTATUS(status) == 37);
    assert(LSBChildStatusTryWait(&ledger, -1, 0, &pid, &status) == ECHILD);
    LSBChildTicket reused = running(12, 7);
    assert(reused.index == first.index && reused.generation != first.generation);
    assert(LSBChildStatusPublishExit(&ledger, first, 0) == ESTALE);
    assert(LSBChildStatusAbandon(&ledger, first) == ESTALE);
    assert(LSBChildStatusTryWait(&ledger, 12, 0, &pid, &status) == EAGAIN);
    assert(!LSBChildStatusPublishExit(&ledger, reused, 0));
    assert(!LSBChildStatusTryWait(&ledger, -1, 0, &pid, NULL) && pid == 12);
    assert(LSBChildStatusTryWait(&ledger, INT_MIN, 0, &pid, NULL) == ECHILD);
}

static void testCapacityAndDiscard(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket tickets[LSB_CHILD_STATUS_CAPACITY], extra;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) tickets[i] = running((pid_t)i + 1, 1);
    assert(LSBChildStatusReserve(&ledger, &extra) == EAGAIN);
    assert(!LSBChildStatusAbandon(&ledger, tickets[4]));
    assert(!LSBChildStatusReserve(&ledger, &extra) && extra.index == tickets[4].index);
    assert(!LSBChildStatusAbandon(&ledger, extra));
    assert(!LSBChildStatusPublishExit(&ledger, tickets[0], 1 << 8));
    LSBChildStatusSetDiscard(&ledger, 1);
    assert(!LSBChildStatusPublishExit(&ledger, tickets[1], 2 << 8));
    pid_t pid;
    int status;
    assert(LSBChildStatusTryWait(&ledger, 1, 0, &pid, &status) == ECHILD);
    assert(LSBChildStatusTryWait(&ledger, 2, 0, &pid, &status) == ECHILD);
    LSBChildStatusSetDiscard(&ledger, 0);
    assert(!LSBChildStatusPublishExit(&ledger, tickets[2], 3 << 8));
    assert(!LSBChildStatusTryWait(&ledger, 3, 0, &pid, &status) && WEXITSTATUS(status) == 3);
    ledger.version = 99;
    assert(LSBChildStatusValidate(&ledger) == EPROTO);
    ledger.version = LSB_CHILD_STATUS_VERSION;
    atomic_store(&ledger.nextGeneration, UINT64_MAX);
    assert(LSBChildStatusReserve(&ledger, &extra) == EOVERFLOW && atomic_load(&ledger.nextGeneration) == UINT64_MAX);
}

static _Atomic(unsigned) gate, winners;
static void *competingWaiter(void *unused) {
    (void)unused;
    while (!atomic_load(&gate)) sched_yield();
    pid_t pid;
    int status;
    int result = LSBChildStatusTryWait(&ledger, 42, 0, &pid, &status);
    assert(result == 0 || result == ECHILD);
    if (!result) { assert(pid == 42 && status == 0); atomic_fetch_add(&winners, 1); }
    return NULL;
}

static void testConcurrentReaping(void) {
    LSBChildStatusInitialize(&ledger);
    LSBChildTicket ticket = running(42, 1);
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    pthread_t threads[8];
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_create(&threads[i], NULL, competingWaiter, NULL));
    atomic_store(&gate, 1);
    for (unsigned i = 0; i < 8; ++i) assert(!pthread_join(threads[i], NULL));
    assert(atomic_load(&winners) == 1);
}

enum { StressCount = 4000 };
static void *reuseProducer(void *unused) {
    (void)unused;
    for (int i = 0; i < StressCount; ++i) {
        LSBChildTicket ticket;
        int error;
        do { error = LSBChildStatusReserve(&ledger, &ticket); if (error == EAGAIN) sched_yield(); } while (error == EAGAIN);
        assert(!error && !LSBChildStatusPublishRunning(&ledger, ticket, 100 + i, 9));
        assert(!LSBChildStatusPublishExit(&ledger, ticket, (i & 255) << 8));
    }
    return NULL;
}

static void testConcurrentReuse(void) {
    LSBChildStatusInitialize(&ledger);
    pthread_t producer;
    assert(!pthread_create(&producer, NULL, reuseProducer, NULL));
    unsigned reaped = 0;
    while (reaped < StressCount) {
        pid_t pid;
        int status;
        int result = LSBChildStatusTryWait(&ledger, -1, 0, &pid, &status);
        if (!result) { assert(pid >= 100 && pid < 100 + StressCount && WEXITSTATUS(status) == ((pid - 100) & 255)); ++reaped; }
        else { assert(result == EAGAIN || result == ECHILD); sched_yield(); }
    }
    assert(!pthread_join(producer, NULL));
}

enum { PolicyRounds = 4000 };
static _Atomic(unsigned) policyRound, policyFinished;
static LSBChildTicket policyTicket;
static void *racePolicyPublication(void *pointer) {
    int publish = pointer != NULL;
    for (unsigned round = 1; round <= PolicyRounds; ++round) {
        while (atomic_load_explicit(&policyRound, memory_order_acquire) != round) sched_yield();
        if (publish) assert(!LSBChildStatusPublishExit(&ledger, policyTicket, 17 << 8));
        else LSBChildStatusSetDiscard(&ledger, 1);
        atomic_fetch_add_explicit(&policyFinished, 1, memory_order_release);
    }
    return NULL;
}

static void testConcurrentDiscard(void) {
    pthread_t publisher, policy;
    assert(!pthread_create(&publisher, NULL, racePolicyPublication, &ledger));
    assert(!pthread_create(&policy, NULL, racePolicyPublication, NULL));
    for (unsigned round = 1; round <= PolicyRounds; ++round) {
        LSBChildStatusInitialize(&ledger);
        policyTicket = running(55, 1);
        atomic_store_explicit(&policyFinished, 0, memory_order_relaxed);
        atomic_store_explicit(&policyRound, round, memory_order_release);
        while (atomic_load_explicit(&policyFinished, memory_order_acquire) != 2) sched_yield();
        pid_t pid;
        int status;
        assert(LSBChildStatusTryWait(&ledger, -1, 0, &pid, &status) == ECHILD);
    }
    assert(!pthread_join(publisher, NULL) && !pthread_join(policy, NULL));
}

static void testSharedAcrossProcesses(void) {
    LSBChildStatus *shared = mmap(NULL, LSB_CHILD_STATUS_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    assert(shared != MAP_FAILED);
    LSBChildStatusInitialize(shared);
    LSBChildTicket ticket;
    assert(!LSBChildStatusReserve(shared, &ticket));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(!LSBChildStatusPublishRunning(shared, ticket, getpid(), getpgrp()));
        assert(!LSBChildStatusPublishExit(shared, ticket, 23 << 8));
        _exit(0);
    }
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    pid_t reported;
    assert(!LSBChildStatusTryWait(shared, child, 0, &reported, &status) && reported == child && WEXITSTATUS(status) == 23);
    assert(!munmap(shared, LSB_CHILD_STATUS_MAP_SIZE));
}

int main(void) {
    alarm(15);
    testSelectionAndReuse();
    testCapacityAndDiscard();
    testConcurrentReaping();
    testConcurrentReuse();
    testConcurrentDiscard();
    testSharedAcrossProcesses();
    puts("ChildStatus: PID/group selection, status, capacity, discard policy, stale generations, concurrent reaping/reuse and shared mapping passed");
    return 0;
}
