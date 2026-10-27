#include "ProcessDisplayName.h"

#include <errno.h>
#include <string.h>
#include <sys/sysctl.h>

int LSBSetProcessDisplayName(const char *path, char *nativeArgv0, size_t argv0Capacity) {
    int savedErrno = errno;
    if (!path || !*path || (argv0Capacity && !nativeArgv0)) return EINVAL;
    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    if (!*leaf) return EINVAL;
    char name[33];
    size_t length = strnlen(leaf, sizeof(name) - 1);
    memcpy(name, leaf, length);
    name[length] = 0;
    // Only touch the original argv[0] allocation. Adjacent native arguments, env and apple vectors remain intact.
    // These bytes survive for native roles; simulator handoff may replace the old native stack with cache mappings.
    if (argv0Capacity) {
        size_t copied = length < argv0Capacity - 1 ? length : argv0Capacity - 1;
        memcpy(nativeArgv0, name, copied);
        memset(nativeArgv0 + copied, 0, argv0Capacity - copied);
    }
    // XNU kern_sysctl.c sysctl_procname stores this in p_name, independently of the user stack and signed executable path.
    int mib[] = {CTL_KERN, KERN_PROCNAME};
    int error = sysctl(mib, 2, NULL, NULL, name, length + 1) ? errno : 0;
    errno = savedErrno;
    return error;
}
