#ifndef LSB_EXTENSION_KIT_REGISTRATION_H
#define LSB_EXTENSION_KIT_REGISTRATION_H

#include <stdbool.h>

// Register the fixed runtime ExtensionKit service bundle only for an explicitly enabled, canonical SpringBoard executable.
// Returns errno on validation failure; requested means the void XPC call returned, not that the service was launched.
int LSBRegisterExtensionKitForExecutable(const char *enabled, const char *runtimeRoot, const char *executablePath, bool *requested);

#endif
