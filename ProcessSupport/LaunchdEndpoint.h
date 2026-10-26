#ifndef LSB_LAUNCHD_ENDPOINT_H
#define LSB_LAUNCHD_ENDPOINT_H

#include <mach/mach.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
// Registers a borrowed send right in this simulator launchd's bridge namespace, using the 23A343 MIG 701 request.
// No system bootstrap registration or XPC connection is involved. Call away from the host's main queue.
// Waits at most one second; returns errno values and preserves errno. A timeout leaves registration outcome unknown.
// bootstrapStatus receives -1 unless an authenticated reply supplies the server's return code. Both input rights stay borrowed.
int LSBLaunchdRegisterEndpoint(mach_port_t service, const audit_token_t *expectedAudit, const char *name,
                              mach_port_t endpoint, kern_return_t *bootstrapStatus);
// Same transaction and ownership rules, with a caller-selected 1..30000 millisecond budget shared by send and receive.
// Keep one request alive while startup is busy; never retry an unknown registration after a timeout.
int LSBLaunchdRegisterEndpointWithTimeout(mach_port_t service, const audit_token_t *expectedAudit, const char *name,
                                         mach_port_t endpoint, uint32_t timeoutMilliseconds, kern_return_t *bootstrapStatus);
// Stock 23A343 bridge removal (MIG 709); same authentication, borrowed service right and bounded transaction rules.
// ESRCH means an authenticated reply confirmed that the name was already absent. Never retry a timed-out removal blindly.
int LSBLaunchdUnregisterEndpointWithTimeout(mach_port_t service, const audit_token_t *expectedAudit, const char *name,
                                           uint32_t timeoutMilliseconds, kern_return_t *bootstrapStatus);
#ifdef __cplusplus
}
#endif

#endif
