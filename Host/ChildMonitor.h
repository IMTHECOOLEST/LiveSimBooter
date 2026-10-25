#pragma once

#include <mach/mach.h>
#include <sys/types.h>

// Register before the child receives its bootstrap plan: EVFILT_PROC does not replay an earlier exit.
// On success the caller owns a close-on-exec kqueue descriptor, readable when a raw wait status is available.
int LSBChildMonitorCreate(pid_t pid, int *descriptor);
int LSBChildMonitorRead(int descriptor, pid_t pid, int *waitStatus);
// Signals only the original PID generation. Returns an errno value, never targets a reused PID.
int LSBChildSignal(const audit_token_t *audit, int signal);
