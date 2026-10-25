#pragma once

#include "SpawnTransport.h"

// Native host only. Requires a zeroed plan and message->fds initialized to -1; leaves them unchanged on failure.
// Creates only owned SimulatorData paths beside RuntimeRoot, regenerating its bootstrap plist and appending to its regular log file.
// The caller supplies signal masks after success and owns the returned FDs. No terminal is attached.
int LSBPrepareLaunchdPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]);

// Opt-in launchd probe: the bootstrap loads one generated RunAtLoad df job from an otherwise empty owned directory.
// The executable path is absolute inside RuntimeRoot; this does not provide a chroot or load any runtime LaunchDaemons.
int LSBPrepareLaunchdDFPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]);

// One-shot stock backboardd probe, preserving its MachServices but omitting unsupported native policy attributes.
int LSBPrepareLaunchdBackboardPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]);
// Process-event and orphan-adoption regression job under simulator launchd.
int LSBPrepareLaunchdLifecyclePlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]);
