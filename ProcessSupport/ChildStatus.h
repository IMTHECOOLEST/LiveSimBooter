#ifndef LSB_CHILD_STATUS_H
#define LSB_CHILD_STATUS_H

#include <stdatomic.h>
#include <stdint.h>
#include <sys/types.h>

#define LSB_CHILD_STATUS_MAP_SIZE 4096
#define LSB_CHILD_STATUS_MAPPING_SIZE LSB_CHILD_STATUS_MAP_SIZE
#define LSB_CHILD_STATUS_ENV "LSB_CHILD_STATUS"
#define LSB_CHILD_WAKE_FD_ENV "LSB_CHILD_WAKE_FD"
#define LSB_CHILD_STATUS_MAGIC 0x4c534243u
#define LSB_CHILD_STATUS_VERSION 1u
#define LSB_CHILD_STATUS_CAPACITY 16u

typedef struct {
    _Atomic(uint64_t) state;
    _Atomic(int32_t) pid, group, status;
} LSBChildSlot;

typedef struct {
    uint32_t magic, version, capacity, reserved;
    _Atomic(uint64_t) nextGeneration;
    _Atomic(uint32_t) discardExited;
    LSBChildSlot slots[LSB_CHILD_STATUS_CAPACITY];
} LSBChildStatus;

typedef struct { uint32_t index; uint64_t generation; } LSBChildTicket;

void LSBChildStatusInitialize(LSBChildStatus *ledger);
int LSBChildStatusValidate(const LSBChildStatus *ledger);
// Reservations consume a new generation; reaped slots are reusable. No pointers cross a process boundary.
int LSBChildStatusReserve(LSBChildStatus *ledger, LSBChildTicket *ticket);
int LSBChildStatusPublishRunning(LSBChildStatus *ledger, LSBChildTicket ticket, pid_t pid, pid_t group);
int LSBChildStatusPublishExit(LSBChildStatus *ledger, LSBChildTicket ticket, int waitStatus);
int LSBChildStatusAbandon(LSBChildStatus *ledger, LSBChildTicket ticket);
// 0: one child reaped; EAGAIN: matching live child; ECHILD: none. Outputs change only on success.
int LSBChildStatusTryWait(LSBChildStatus *ledger, pid_t selector, pid_t currentGroup, pid_t *pid, int *status);
// SIG_IGN and SA_NOCLDWAIT discard both pending and future statuses. Uses only lock-free atomics.
void LSBChildStatusSetDiscard(LSBChildStatus *ledger, int discard);

#endif
