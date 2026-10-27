#ifndef LSB_METAL_COMPILER_REGISTRATION_H
#define LSB_METAL_COMPILER_REGISTRATION_H

#include <stdbool.h>

// Register the fixed compiler bundle for enabled canonical graphics roles or declared main executables of contained stock app bundles.
// Returns errno on validation failure; requested means the void XPC call returned, not that the compiler was launched.
int LSBRegisterMetalCompilerForExecutable(const char *enabled, const char *runtimeRoot, const char *executablePath, bool *requested);

#endif
