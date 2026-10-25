#ifndef LSB_LAUNCHD_ENDPOINT_H
#define LSB_LAUNCHD_ENDPOINT_H

#include <mach/mach.h>

#ifdef __cplusplus
extern "C" {
#endif
// Registers a borrowed send right in this simulator launchd's bridge namespace, using the 23A343 MIG 701 request.
// No system bootstrap registration or XPC connection is involved. Call away from the host's main queue.
// Waits at most one second; returns errno values and preserves errno. A timeout leaves registration outcome unknown.
// bootstrapStatus receives -1 unless an authenticated reply supplies the server's return code. Both input rights stay borrowed.
int LSBLaunchdRegisterEndpoint(mach_port_t service, const audit_token_t *expectedAudit, const char *name,
                              mach_port_t endpoint, kern_return_t *bootstrapStatus);
#ifdef __cplusplus
}
#endif

#endif
