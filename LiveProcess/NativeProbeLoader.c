#include "NativeProbeLoader.h"

#include <dlfcn.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int reportError(int error, const char *operation, const char *reason, char *detail, size_t size) {
    if (detail && size) snprintf(detail, size, "%s: %s", operation, reason ? reason : strerror(error));
    return error;
}

static int isInside(const char *path, const char *directory) {
    size_t length = strlen(directory);
    return !strncmp(path, directory, length) && path[length] == '/';
}

int LSBNativeProbeResolvePaths(const char *executablePath, char folder[PATH_MAX], char library[PATH_MAX]) {
    if (!executablePath || executablePath[0] != '/' || !folder || !library) return EINVAL;
    folder[0] = library[0] = 0;
    char executable[PATH_MAX], candidate[PATH_MAX];
    if (!realpath(executablePath, executable)) return errno;
    struct stat metadata;
    if (stat(executable, &metadata)) return errno;
    if (!S_ISREG(metadata.st_mode)) return EINVAL;
    char *separator = strrchr(executable, '/');
    if (!separator || separator == executable) return EINVAL;
    *separator = 0;
    int length = snprintf(candidate, sizeof(candidate), "%s/Frameworks/NativeMetal", executable);
    if (length < 0 || (size_t)length >= sizeof(candidate)) return ENAMETOOLONG;
    if (!realpath(candidate, folder)) return errno;
    if (!isInside(folder, executable)) return EACCES;
    if (stat(folder, &metadata)) return errno;
    if (!S_ISDIR(metadata.st_mode)) return ENOTDIR;
    length = snprintf(candidate, sizeof(candidate), "%s/MetalNativeProbe.dylib", folder);
    if (length < 0 || (size_t)length >= sizeof(candidate)) return ENAMETOOLONG;
    if (!realpath(candidate, library)) return errno;
    if (!isInside(library, folder)) return EACCES;
    if (stat(library, &metadata)) return errno;
    return S_ISREG(metadata.st_mode) ? 0 : EINVAL;
}

int LSBNativeProbePrepareForExecutable(const char *executablePath, LSBNativeProbe *probe, char *detail, size_t detailSize) {
    if (detail && detailSize) detail[0] = 0;
    if (!probe) return reportError(EINVAL, "native probe", NULL, detail, detailSize);
    memset(probe, 0, sizeof(*probe));
    char library[PATH_MAX];
    int error = LSBNativeProbeResolvePaths(executablePath, probe->folder, library);
    if (error) return reportError(error, "Frameworks/NativeMetal/MetalNativeProbe.dylib", NULL, detail, detailSize);
    probe->handle = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!probe->handle) return reportError(ENOEXEC, "native backend dlopen", dlerror(), detail, detailSize);
    dlerror();
    void *symbol = dlsym(probe->handle, "LSBMetalNativeProbe");
    const char *symbolError = dlerror();
    if (!symbol || symbolError) return reportError(ENOEXEC, "LSBMetalNativeProbe", symbolError, detail, detailSize);
    Dl_info provider = {0};
    char providerPath[PATH_MAX];
    if (!dladdr(symbol, &provider) || !provider.dli_fname || !realpath(provider.dli_fname, providerPath) || strcmp(providerPath, library))
        return reportError(EXDEV, "LSBMetalNativeProbe", "symbol is not defined by the fixed backend image", detail, detailSize);
    probe->entry = (int (*)(const char *))symbol;
    return 0;
}

int LSBNativeProbePrepare(LSBNativeProbe *probe, char *detail, size_t detailSize) {
    char executable[PATH_MAX];
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size))
        return reportError(ENAMETOOLONG, "native executable path", NULL, detail, detailSize);
    return LSBNativeProbePrepareForExecutable(executable, probe, detail, detailSize);
}

int LSBNativeProbeRun(const LSBNativeProbe *probe) {
    if (!probe || !probe->handle || !probe->entry || probe->folder[0] != '/') return EINVAL;
    int result = probe->entry(probe->folder);
    return result < 0 ? EPROTO : result;
}
