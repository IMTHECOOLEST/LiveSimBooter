#ifndef LSB_NATIVE_PROBE_LOADER_H
#define LSB_NATIVE_PROBE_LOADER_H

#include <limits.h>
#include <stddef.h>

typedef struct {
    void *handle;
    int (*entry)(const char *absoluteFolder);
    char folder[PATH_MAX];
} LSBNativeProbe;

// Resolve only the fixed Frameworks/NativeMetal location beneath the canonical extension executable directory.
// The explicit executable argument supports filesystem tests; production Prepare obtains it from native dyld itself.
int LSBNativeProbeResolvePaths(const char *executablePath, char folder[PATH_MAX], char library[PATH_MAX]);
int LSBNativeProbePrepareForExecutable(const char *executablePath, LSBNativeProbe *probe, char *detail, size_t detailSize);
int LSBNativeProbePrepare(LSBNativeProbe *probe, char *detail, size_t detailSize);
int LSBNativeProbeRun(const LSBNativeProbe *probe);
// Loaded Objective-C backend images remain retained until process exit, including after a preparation failure.

#endif
