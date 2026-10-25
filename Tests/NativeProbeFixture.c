#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(LSB_NATIVE_PROBE_MISSING_ENTRY)
#define FixtureEntry LSBUnrelatedProbe
#else
#define FixtureEntry LSBMetalNativeProbe
#endif

int FixtureEntry(const char *folder) {
    if (!folder || folder[0] != '/') return EINVAL;
    char path[PATH_MAX];
    int count = snprintf(path, sizeof(path), "%s/probe-result", folder);
    if (count < 0 || (size_t)count >= sizeof(path)) return ENAMETOOLONG;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return errno;
    size_t length = strlen(folder);
    int result = write(fd, folder, length) == (ssize_t)length ? 0 : EIO;
    if (close(fd) && !result) result = errno;
    count = snprintf(path, sizeof(path), "%s/return-error", folder);
    if (count < 0 || (size_t)count >= sizeof(path)) return ENAMETOOLONG;
    return access(path, F_OK) == 0 ? 37 : result;
}
