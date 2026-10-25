#pragma once

#include "MainThreadBootstrap.h"

struct mach_header;

// Query the real process policy before cache removal; no host function pointer survives in the callback table.
void LSBPrepareHostPolicy();

// Prepare app-owned bootstrap storage while host pthread APIs remain available; does not switch TLS yet.
int LSBPrepareHostMainThread(lsb::MainThreadBootstrap *bootstrap);

// Snapshot native debugger metadata and the shared-cache range before the handoff changes either.
int LSBPrepareHostDebugger(const mach_header *executable, const char *executablePath);

// Reject any remaining native worker immediately before switching TLS and removing its code mappings.
int LSBValidateHostThreadCount();
