#pragma once

#include <stdbool.h>

// Icon launches have only argv[0]. Preserve automatic execution for explicit development/test arguments, including invalid plans.
static inline bool LSBHostArgumentsRequestAutomaticStart(int argc, const char *const argv[]) {
    return argc > 1 && argv && argv[0] && argv[1];
}

int LSBRunHostApplication(int argc, const char *argv[], const char *envp[]);
bool LSBInitialGuestStartsAutomatically(void);
bool LSBInitialGuestStartupWasAttempted(void);
// Main-thread, once-only startup. Failure remains latched because the broker may already own its service and callbacks.
int LSBStartInitialGuest(void);
