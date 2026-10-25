#ifndef LSB_LAUNCHD_PROBE_H
#define LSB_LAUNCHD_PROBE_H

#include <mach/mach.h>

#ifdef __cplusplus
extern "C" {
#endif
// Native host only. Queries a deliberately missing service with legacy MIG 404, directly on the supplied borrowed send right.
// Uses no XPC or worker threads and waits at most one second in total. Call away from the host's main queue.
// Returns errno values, preserving errno. Success requires an authenticated BOOTSTRAP_UNKNOWN_SERVICE reply.
// bootstrapStatus is optional; it receives -1 until an exact, authenticated reply supplies its bootstrap return code.
int LSBLaunchdProbeService(mach_port_t service, const audit_token_t *expectedAudit, kern_return_t *bootstrapStatus);
#ifdef __cplusplus
}
#endif

#endif
