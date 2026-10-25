#include "../Guest/ChildWait.h"

#include <assert.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

// Link this client against a test dylib containing ProcessEvents, ProcessIdentity, ChildWait and ChildStatus with
// LSB_CHILD_WAIT_TESTING defined. Calls below deliberately use the normal libdispatch symbol names.
static dispatch_source_t source;
static dispatch_semaphore_t events, cancellation;
static _Atomic(unsigned long) observed;
static void event(void *unused) {
    (void)unused;
    assert(dispatch_source_get_handle(source) == 600000);
    assert(dispatch_source_get_mask(source) == (DISPATCH_PROC_EXEC | DISPATCH_PROC_EXIT));
    atomic_fetch_or(&observed, dispatch_source_get_data(source));
    dispatch_semaphore_signal(events);
}
static void cancel(void *unused) { (void)unused; dispatch_semaphore_signal(cancellation); }
static void waitFor(dispatch_semaphore_t semaphore) {
    assert(!dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)));
}

int main(void) {
    alarm(10);
    LSBChildStatus ledger;
    LSBChildStatusInitialize(&ledger);
    int descriptors[2];
    assert(!pipe(descriptors) && !fcntl(descriptors[0], F_SETFL, O_NONBLOCK));
    assert(!LSBChildWaitConfigure(&ledger, descriptors[0]));
    LSBChildTicket ticket;
    assert(!LSBChildStatusReserve(&ledger, &ticket));
    assert(!LSBChildStatusPublishRunning(&ledger, ticket, 600000, 600000));
    events = dispatch_semaphore_create(0);
    cancellation = dispatch_semaphore_create(0);
    source = dispatch_source_create(DISPATCH_SOURCE_TYPE_PROC, 600000, DISPATCH_PROC_EXEC | DISPATCH_PROC_EXIT,
                                    dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0));
    assert(source);
    dispatch_retain(source);
    dispatch_release(source);
    dispatch_source_set_event_handler_f(source, event);
    dispatch_source_set_cancel_handler_f(source, cancel);
    dispatch_activate(source);
    assert(!LSBChildStatusPublishExec(&ledger, ticket, 600001));
    while (!(atomic_load(&observed) & DISPATCH_PROC_EXEC)) waitFor(events);
    assert(!LSBChildStatusPublishExit(&ledger, ticket, 0));
    while (!(atomic_load(&observed) & DISPATCH_PROC_EXIT)) waitFor(events);
    dispatch_source_cancel(source);
    waitFor(cancellation);
    dispatch_release(source);
    dispatch_release(events);
    dispatch_release(cancellation);
    close(descriptors[0]);
    close(descriptors[1]);
    puts("ProcessEvents: normal imported libdispatch symbols interposed successfully");
    return 0;
}
