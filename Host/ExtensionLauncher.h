#pragma once

#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

// Events run on the main thread. A positive PID comes from pkd's launch reply; an error is terminal for this request.
// No child NSXPC extension-context handshake is required. Broker check-in establishes guest readiness separately.
typedef void (*LSBExtensionEvent)(uint64_t requestID, pid_t pid, int error, void *context);

// Call on the main thread. Returns an errno value for an immediate failure, with no callback for that failure.
// A zero return only means the request was submitted. Keep context alive until LSBReleaseExtension returns.
int LSBLaunchExtension(const char *identifier, uint64_t requestID, LSBExtensionEvent event, void *context);

// Call on the main thread. Returns the PID supplied by pkd after its launch reply, or 0 if unavailable.
pid_t LSBExtensionProcessIdentifier(uint64_t requestID);

// Call on the main thread. Stops callbacks and releases the retained launch and process assertion; does not send a termination request.
void LSBReleaseExtension(uint64_t requestID);

#ifdef __cplusplus
}
#endif
