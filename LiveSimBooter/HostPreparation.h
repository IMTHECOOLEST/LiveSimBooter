#pragma once

#include "MainThreadBootstrap.h"

// Query the real process policy before cache removal; no host function pointer survives in the callback table.
void LSBPrepareHostPolicy();

// Prepare app-owned bootstrap storage while host pthread APIs remain available; does not switch TLS yet.
int LSBPrepareHostMainThread(lsb::MainThreadBootstrap *bootstrap);
