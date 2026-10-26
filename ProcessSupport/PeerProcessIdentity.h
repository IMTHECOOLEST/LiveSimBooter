#ifndef LSB_PEER_PROCESS_IDENTITY_H
#define LSB_PEER_PROCESS_IDENTITY_H

#include "SpawnPlan.h"
#include <mach/mach.h>
#include <string.h>

enum { LSB_PEER_SHORT_INFO_BYTES = 64, LSB_PEER_SHORT_INFO_FLAVOR = 13 };

// Native kernel identity only. The broker supplies the guest executable path; this is not a stable guest PID mapping.
typedef struct {
    audit_token_t audit;
    unsigned char shortInfo[LSB_PEER_SHORT_INFO_BYTES];
    char executablePath[LSB_PATH_BYTES];
    mach_port_t taskName; // One owned name-only task send reference, never a task-control right.
} LSBPeerProcessIdentity;

// PROC_PIDT_SHORTBSDINFO starts with uint32_t pbsi_pid. Read through memcpy because the wire bytes need not be aligned.
static inline uint32_t LSBPeerProcessIdentityPID(const LSBPeerProcessIdentity *identity) {
    uint32_t pid = 0;
    if (identity) memcpy(&pid, identity->shortInfo, sizeof(pid));
    return pid;
}

// Accept only initialized values. A successful transport result owns its taskName; zeroed/error results are safe to dispose.
static inline void LSBPeerProcessIdentityDispose(LSBPeerProcessIdentity *identity) {
    if (!identity) return;
    if (MACH_PORT_VALID(identity->taskName)) mach_port_deallocate(mach_task_self(), identity->taskName);
    memset(identity, 0, sizeof(*identity));
}

#endif
