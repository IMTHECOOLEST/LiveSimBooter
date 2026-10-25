#include "ChildStatus.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>

enum { Empty, Reserved, Running, Exited, Reaped, Publishing, Exiting, PhaseMask = 7 };
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2 && ATOMIC_LONG_LOCK_FREE == 2 && ATOMIC_INT_LOCK_FREE == 2,
               "Child status access must remain lock-free in a signal handler");
_Static_assert(sizeof(LSBChildStatus) <= LSB_CHILD_STATUS_MAP_SIZE, "Child status mapping is too small");

static uint64_t tag(LSBChildTicket ticket, unsigned phase) { return (ticket.generation << 3) | phase; }
static int validTicket(LSBChildTicket ticket) {
    return ticket.index < LSB_CHILD_STATUS_CAPACITY && ticket.generation && ticket.generation <= (UINT64_MAX >> 3);
}

void LSBChildStatusInitialize(LSBChildStatus *ledger) {
    ledger->magic = LSB_CHILD_STATUS_MAGIC;
    ledger->version = LSB_CHILD_STATUS_VERSION;
    ledger->capacity = LSB_CHILD_STATUS_CAPACITY;
    ledger->reserved = 0;
    atomic_init(&ledger->nextGeneration, 1);
    atomic_init(&ledger->discardExited, 0);
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        atomic_init(&ledger->slots[i].state, Empty);
        atomic_init(&ledger->slots[i].pid, 0);
        atomic_init(&ledger->slots[i].group, 0);
        atomic_init(&ledger->slots[i].status, 0);
    }
}

int LSBChildStatusValidate(const LSBChildStatus *ledger) {
    if (!ledger || (uintptr_t)ledger % _Alignof(LSBChildStatus) || ledger->magic != LSB_CHILD_STATUS_MAGIC ||
        ledger->version != LSB_CHILD_STATUS_VERSION || ledger->capacity != LSB_CHILD_STATUS_CAPACITY || ledger->reserved) return EPROTO;
    return 0;
}

int LSBChildStatusReserve(LSBChildStatus *ledger, LSBChildTicket *ticket) {
    if (!ticket || LSBChildStatusValidate(ledger)) return EINVAL;
    uint64_t generation = atomic_load_explicit(&ledger->nextGeneration, memory_order_relaxed);
    for (;;) {
        if (!generation || generation > (UINT64_MAX >> 3)) return EOVERFLOW;
        if (atomic_compare_exchange_weak_explicit(&ledger->nextGeneration, &generation, generation + 1,
                                                memory_order_relaxed, memory_order_relaxed)) break;
    }
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        uint64_t state = atomic_load_explicit(&ledger->slots[i].state, memory_order_acquire);
        while ((state & PhaseMask) == Empty || (state & PhaseMask) == Reaped) {
            uint64_t reserved = (generation << 3) | Reserved;
            if (atomic_compare_exchange_weak_explicit(&ledger->slots[i].state, &state, reserved,
                                                     memory_order_acq_rel, memory_order_acquire)) {
                *ticket = (LSBChildTicket){i, generation};
                return 0;
            }
        }
    }
    return EAGAIN;
}

int LSBChildStatusPublishRunning(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t pid, pid_t group) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket) || pid <= 0 || group < 0) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    uint64_t expected = tag(ticket, Reserved);
    if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected, tag(ticket, Publishing),
                                                memory_order_acq_rel, memory_order_acquire)) return ESTALE;
    atomic_store_explicit(&slot->pid, pid, memory_order_relaxed);
    atomic_store_explicit(&slot->group, group, memory_order_relaxed);
    atomic_store_explicit(&slot->status, 0, memory_order_relaxed);
    atomic_store_explicit(&slot->state, tag(ticket, Running), memory_order_release);
    return 0;
}

int LSBChildStatusPublishExit(LSBChildStatus *ledger, LSBChildTicket ticket, int waitStatus) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket) || waitStatus < 0 || waitStatus > UINT16_MAX) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    uint64_t expected = tag(ticket, Running);
    if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected, tag(ticket, Exiting),
                                                memory_order_acq_rel, memory_order_acquire)) return ESTALE;
    atomic_store_explicit(&slot->status, waitStatus, memory_order_relaxed);
    // These SC operations pair with SetDiscard's store+scan: neither side may miss the other's publication on ARM.
    atomic_store_explicit(&slot->state, tag(ticket, Exited), memory_order_seq_cst);
    if (atomic_load_explicit(&ledger->discardExited, memory_order_seq_cst)) {
        expected = tag(ticket, Exited);
        atomic_compare_exchange_strong_explicit(&slot->state, &expected, tag(ticket, Reaped),
                                               memory_order_acq_rel, memory_order_acquire);
    }
    return 0;
}

int LSBChildStatusAbandon(LSBChildStatus *ledger, LSBChildTicket ticket) {
    if (LSBChildStatusValidate(ledger) || !validTicket(ticket)) return EINVAL;
    LSBChildSlot *slot = &ledger->slots[ticket.index];
    uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
    for (;;) {
        if (state != tag(ticket, Reserved) && state != tag(ticket, Running)) return ESTALE;
        if (atomic_compare_exchange_weak_explicit(&slot->state, &state, tag(ticket, Reaped),
                                                 memory_order_acq_rel, memory_order_acquire)) return 0;
    }
}

static int matches(pid_t selector, pid_t currentGroup, pid_t pid, pid_t group) {
    if (selector > 0) return pid == selector;
    if (selector == -1) return 1;
    return group == (selector == 0 ? (int64_t)currentGroup : -(int64_t)selector);
}

int LSBChildStatusTryWait(LSBChildStatus *ledger, pid_t selector, pid_t currentGroup, pid_t *pid, int *status) {
    if (!ledger || !pid) return EINVAL;
    int live = 0;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        LSBChildSlot *slot = &ledger->slots[i];
        uint64_t state = atomic_load_explicit(&slot->state, memory_order_acquire);
        for (;;) {
            unsigned phase = state & PhaseMask;
            if (phase == Reserved || phase == Publishing) { if (selector == -1) live = 1; break; }
            if (phase != Running && phase != Exiting && phase != Exited) break;
            pid_t child = atomic_load_explicit(&slot->pid, memory_order_relaxed);
            pid_t group = atomic_load_explicit(&slot->group, memory_order_relaxed);
            int value = atomic_load_explicit(&slot->status, memory_order_relaxed);
            // Fields can change when another waiter reaps and the host reuses a slot. Never match a mixed generation.
            uint64_t confirmed = atomic_load_explicit(&slot->state, memory_order_acquire);
            if (confirmed != state) { state = confirmed; continue; }
            if (!matches(selector, currentGroup, child, group)) break;
            if (phase != Exited) { live = 1; break; }
            uint64_t reaped = (state & ~(uint64_t)PhaseMask) | Reaped;
            int discard = atomic_load_explicit(&ledger->discardExited, memory_order_seq_cst) != 0;
            if (!atomic_compare_exchange_weak_explicit(&slot->state, &state, reaped, memory_order_acq_rel, memory_order_acquire)) continue;
            if (discard) break;
            *pid = child;
            if (status) *status = value;
            return 0;
        }
    }
    return live ? EAGAIN : ECHILD;
}

void LSBChildStatusSetDiscard(LSBChildStatus *ledger, int discard) {
    if (!ledger) return;
    atomic_store_explicit(&ledger->discardExited, discard != 0, memory_order_seq_cst);
    if (!discard) return;
    for (unsigned i = 0; i < LSB_CHILD_STATUS_CAPACITY; ++i) {
        LSBChildSlot *slot = &ledger->slots[i];
        uint64_t state = atomic_load_explicit(&slot->state, memory_order_seq_cst);
        while ((state & PhaseMask) == Exited) {
            uint64_t reaped = (state & ~(uint64_t)PhaseMask) | Reaped;
            if (atomic_compare_exchange_weak_explicit(&slot->state, &state, reaped, memory_order_acq_rel, memory_order_acquire)) break;
        }
    }
}
