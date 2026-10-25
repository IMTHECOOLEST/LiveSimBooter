#pragma once

#include "SpawnTransport.h"

// Native host only. Requires a zeroed plan and message->fds initialized to -1; leaves them unchanged on failure.
// Creates only owned SimulatorData paths beside RuntimeRoot, regenerating its bootstrap plist and appending to its regular log file.
// The caller supplies signal masks after success and owns the returned FDs. No terminal is attached.
int LSBPrepareLaunchdPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]);
