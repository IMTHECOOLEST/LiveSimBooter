#pragma once

#include <stdint.h>

namespace lsb {

struct MainThreadBootstrap {
    uintptr_t hostTSD;
    uintptr_t simulatorTSD;
};

// Prepare fresh app-resident pthread/TSD storage without changing the active thread pointer.
int prepareMainThreadBootstrap(MainThreadBootstrap *state, uint64_t threadId);
uintptr_t currentTSDBase();

// Darwin's platform trap has no errno result. Verify the actual register after changing it.
bool setTSDBase(uintptr_t base);

}
