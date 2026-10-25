#ifndef LSB_SPAWN_TRANSPORT_H
#define LSB_SPAWN_TRANSPORT_H

#include "SpawnPlan.h"
#include "SpawnPorts.h"
#include <mach/mach.h>

#define LSB_BROKER_PORT_ENV "LSB_BROKER_PORT"

enum LSBRequestKind {
    LSB_REQUEST_SPAWN = 1, LSB_REQUEST_CHECKIN = 2, LSB_REQUEST_READY = 3, LSB_REQUEST_KILL = 4, LSB_REQUEST_EXEC = 5,
    LSB_REQUEST_RESOLVE_AUDIT_PID = 6
};
typedef struct {
    uint64_t requestID;
    int32_t error, pid, guestPID, parentPID;
    int statusFD, wakeFD;
    mach_port_t launchdTankPort;
    LSBSpawnPorts ports;
    char runtimeRoot[LSB_PATH_BYTES];
    LSBSpawnPlan plan;
} LSBBootstrapReply;

// Borrowed reply metadata. Native PID remains separate from stable guest identity during an exec replacement.
typedef struct {
    pid_t guestPID, parentPID;
    mach_port_t launchdTankPort;
    const LSBSpawnPorts *ports;
} LSBBootstrapMetadata;

typedef struct {
    void *object;
    audit_token_t audit;
    uint32_t kind;
    uint64_t requestID;
    int error;
    pid_t targetPID;
    int signalNumber;
    audit_token_t targetAudit;
    LSBSpawnPlan plan;
    int fds[LSB_MAX_FDS];
    LSBSpawnPorts ports;
} LSBReceivedMessage;

#ifdef __cplusplus
extern "C" {
#endif
// Synchronous transport; no callback queues. The caller owns all returned FDs and registered-port send references.
// The private pipe call has no documented timeout; the broker must deadline pending launches and reply on failure.
// Returns one owned send right. Native callers discover it through bootstrap; the guest copies the handed-off right.
// A native child may keep one raw Mach send right across handoff; no XPC object or native function pointer is retained.
int LSBTransportCopyBrokerPort(mach_port_t *port);
int LSBTransportSpawn(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], pid_t *pid);
// Optional registered/bootstrap ports copied with COPY_SEND. Presence and null slots are preserved; the caller keeps its references.
int LSBTransportSpawnWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports, pid_t *pid);
// Returns an errno value; zero means the broker accepted replacement. The guest exec shim must never return after that success.
int LSBTransportExecWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports);
int LSBTransportCheckIn(LSBBootstrapReply *reply, int fds[LSB_MAX_FDS]);
int LSBTransportReady(uint64_t requestID, int error);
// Uses a borrowed held broker send right, even after native bootstrap/registered ports have been replaced. Caller retains ownership.
int LSBTransportReadyOnPort(mach_port_t broker, uint64_t requestID, int error);
// Exact positive PID of a running broker-managed child. Signal 0 validates identity without sending a signal.
// Returns an errno value. A successful launchd fallback means submitted; waitpid observes actual termination.
int LSBTransportKill(pid_t targetPID, int signalNumber);
// Simulator launchd only: resolve the complete kernel audit identity of a request sender to its committed stable guest PID.
// Unmanaged native identities retain their native PID unless they alias a known managed identity. Never changes the audit token.
// Returns an errno value and clears *pid on failure; rejected or retired identities must not fall back to their native PID.
int LSBTransportResolveAuditPID(const audit_token_t *audit, pid_t *pid);
// Receive retains the request for a later reply; audit comes from the kernel, never the message payload.
// On failure, sends an error reply when possible and disposes the malformed message, imported FDs and copied send references.
int LSBTransportReceive(mach_port_t port, LSBReceivedMessage *message);
// Replies and releases the request object. Imported FDs and registered ports remain owned by message until DisposeMessage is called.
int LSBTransportReply(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]);
// Bootstrap additionally copies a shared status-file FD and nonblocking wake-pipe read FD; caller retains ownership.
// A successful CheckIn returns these in reply.statusFD/wakeFD. The caller must close them separately from the plan FDs.
int LSBTransportReplyBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                               const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD);
// CheckIn returns independent owned send references in reply.ports; release them after applying registered and task-bootstrap ports.
int LSBTransportReplyBootstrapWithPorts(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                        const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, const LSBSpawnPorts *ports);
// guestPID must be positive; parentPID may be zero for a guest root process. Existing wrappers use pid/getpid() respectively.
int LSBTransportReplyBootstrapWithMetadata(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                           const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD,
                                           const LSBBootstrapMetadata *metadata);
// Optional launchd-only temporary bootstrap send right. CheckIn returns an owned send reference; the caller must release it.
int LSBTransportReplyLaunchdBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, mach_port_t tankPort);
// Only pass a message initialized by Receive, and do not reuse it for another Receive before disposal.
void LSBTransportDisposeMessage(LSBReceivedMessage *message);
// Reject handoff if a native transport worker remains; called after all native IPC objects have been released.
int LSBRequireSingleThread(void);
#ifdef __cplusplus
}
#endif
#endif
