#ifndef LSB_LAUNCHD_HANDSHAKE_H
#define LSB_LAUNCHD_HANDSHAKE_H

#include <mach/mach.h>
#include <stdbool.h>

// Native host only. Serialize all calls on the broker queue; this object creates no worker threads.
// Each non-null field owns one right of the indicated kind, even when a send and receive name are numerically equal.
typedef struct {
    mach_port_t tankReceive, tankSend;
    mach_port_t serviceReceive, serviceSend;
    audit_token_t expectedAudit;
    bool completed;
} LSBLaunchdHandshake;

#ifdef __cplusplus
extern "C" {
#endif
// Initialize a fresh or disposed object with the authenticated check-in token, including its PID generation.
// All functions returning int return errno values and preserve errno.
int LSBLaunchdHandshakeInitialize(LSBLaunchdHandshake *state, const audit_token_t *expectedAudit);
// Borrowed send rights. The caller must copy a send uref if retaining or transferring ownership elsewhere.
mach_port_t LSBLaunchdHandshakeTankPort(const LSBLaunchdHandshake *state);
mach_port_t LSBLaunchdHandshakeServicePort(const LSBLaunchdHandshake *state);
// Consume one queued request without waiting. EAGAIN means empty; success transfers the service receive right.
// EACCES/EPROTO/EPIPE reject one request, without completing the handshake. EALREADY means a successful handoff already occurred.
int LSBLaunchdHandshakeHandleNext(LSBLaunchdHandshake *state);
// Close the tank only after its dispatch source has been canceled, normally from that source's cancellation handler.
void LSBLaunchdHandshakeCloseTank(LSBLaunchdHandshake *state);
// Idempotent; call after canceling any dispatch source that watches the tank.
void LSBLaunchdHandshakeDispose(LSBLaunchdHandshake *state);
#ifdef __cplusplus
}
#endif

#endif
