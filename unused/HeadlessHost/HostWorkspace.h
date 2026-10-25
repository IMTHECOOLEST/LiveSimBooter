#pragma once

#include <stdbool.h>

// Call on the main thread before starting broker work, then keep the main run loop running.
// The native host retains its FrontBoard connection for its lifetime; LiveProcess does not use this path.
bool LSBInitializeHostWorkspace(void);
