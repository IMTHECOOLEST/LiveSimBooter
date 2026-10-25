#ifndef LSB_SPAWN_TRANSPORT_H
#define LSB_SPAWN_TRANSPORT_H

#include "SpawnPlan.h"
#include <mach/mach.h>

#define LSB_BROKER_PORT_ENV "LSB_BROKER_PORT"

enum LSBRequestKind { LSB_REQUEST_SPAWN = 1, LSB_REQUEST_CHECKIN = 2, LSB_REQUEST_READY = 3 };
typedef struct {
    uint64_t requestID;
    int32_t error, pid;
    int statusFD, wakeFD;
    char runtimeRoot[LSB_PATH_BYTES];
    LSBSpawnPlan plan;
} LSBBootstrapReply;

typedef struct {
    void *object;
    audit_token_t audit;
    uint32_t kind;
    uint64_t requestID;
    int error;
    LSBSpawnPlan plan;
    int fds[LSB_MAX_FDS];
} LSBReceivedMessage;

#ifdef __cplusplus
extern "C" {
#endif
// Synchronous transport; no callback queues. The caller owns all returned FDs.
// The private pipe call has no documented timeout; the broker must deadline pending launches and reply on failure.
// Returns one owned send right. Native callers discover it through bootstrap; the guest copies the handed-off right.
// A native child may keep one raw Mach send right across handoff; no XPC object or native function pointer is retained.
int LSBTransportCopyBrokerPort(mach_port_t *port);
int LSBTransportSpawn(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], pid_t *pid);
int LSBTransportCheckIn(LSBBootstrapReply *reply, int fds[LSB_MAX_FDS]);
int LSBTransportReady(uint64_t requestID, int error);
// Receive retains the request for a later reply; audit comes from the kernel, never the message payload.
// On failure, sends an error reply when possible and disposes the malformed message and imported FDs.
int LSBTransportReceive(mach_port_t port, LSBReceivedMessage *message);
// Replies and releases the request object. Imported FDs remain owned by message until DisposeMessage is called.
int LSBTransportReply(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]);
// Bootstrap additionally copies a shared status-file FD and nonblocking wake-pipe read FD; caller retains ownership.
// A successful CheckIn returns these in reply.statusFD/wakeFD. The caller must close them separately from the plan FDs.
int LSBTransportReplyBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                               const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD);
// Only pass a message initialized by Receive, and do not reuse it for another Receive before disposal.
void LSBTransportDisposeMessage(LSBReceivedMessage *message);
// Reject handoff if a native transport worker remains; called after all native IPC objects have been released.
int LSBRequireSingleThread(void);
#ifdef __cplusplus
}
#endif
#endif
