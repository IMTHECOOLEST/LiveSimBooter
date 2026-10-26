#include "NotificationSharedMemory.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static const char objectFile[] = "notification-center.shm";

static bool virtualObject(const char *name) {
    // Check the name before getenv: unrelated early libSystem initialization must not consult environment state.
    if (!name || (strcmp(name, "apple.shm.notification_center") && strcmp(name, "/apple.shm.notification_center"))) return false;
    const char *enabled = getenv(LSB_NOTIFICATION_SHM_ENABLE_ENV);
    return enabled && !strcmp(enabled, "1");
}

static int ownedDirectory(int descriptor) {
    struct stat status;
    if (fstat(descriptor, &status)) return errno;
    if (!S_ISDIR(status.st_mode)) return ENOTDIR;
    return status.st_uid == geteuid() ? 0 : EACCES;
}

static int openObjectDirectory(void) {
    const char *root = getenv(LSB_NOTIFICATION_SHM_ROOT_ENV);
    char canonical[PATH_MAX];
    if (!root || root[0] != '/' || !root[1]) { errno = EINVAL; return -1; }
    if (!realpath(root, canonical)) return -1;
    if (strcmp(root, canonical)) { errno = EINVAL; return -1; }
    int directory = open(canonical, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return -1;
    int error = ownedDirectory(directory), temporary = -1;
    if (!error) {
        temporary = openat(directory, "tmp", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        error = temporary < 0 ? errno : ownedDirectory(temporary);
    }
    close(directory);
    if (error) { if (temporary >= 0) close(temporary); errno = error; return -1; }
    return temporary;
}

static int ownedObject(const struct stat *status) {
    if (!S_ISREG(status->st_mode)) return S_ISLNK(status->st_mode) ? ELOOP : EINVAL;
    // An object unlinked concurrently with open can have zero links while its descriptor/mappings remain valid.
    return status->st_uid == geteuid() && status->st_nlink <= 1 ? 0 : EACCES;
}

int LSBNotificationShmOpen(const char *name, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (!virtualObject(name)) return flags & O_CREAT ? shm_open(name, flags, mode) : shm_open(name, flags);
    int access = flags & O_ACCMODE;
    if ((access != O_RDONLY && access != O_RDWR) || (flags & ~(O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_CLOEXEC))) {
        errno = EINVAL; return -1;
    }
    if ((flags & O_TRUNC) && access != O_RDWR) { errno = EINVAL; return -1; }
    int directory = openObjectDirectory();
    if (directory < 0) return -1;
    // Defer truncation until after validation, and use nonblocking open so a substituted FIFO cannot hang startup.
    int descriptor = openat(directory, objectFile, (flags & ~O_TRUNC) | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, mode);
    int error = descriptor < 0 ? errno : 0;
    close(directory);
    if (!error) {
        struct stat status;
        error = fstat(descriptor, &status) ? errno : ownedObject(&status);
        if (!error && (flags & O_TRUNC) && ftruncate(descriptor, 0)) error = errno;
        if (!error) {
            int current = fcntl(descriptor, F_GETFL);
            if (current < 0 || fcntl(descriptor, F_SETFL, current & ~O_NONBLOCK)) error = errno;
        }
    }
    if (error) { if (descriptor >= 0) close(descriptor); errno = error; return -1; }
    return descriptor;
}

int LSBNotificationShmUnlink(const char *name) {
    if (!virtualObject(name)) return shm_unlink(name);
    int directory = openObjectDirectory();
    if (directory < 0) return -1;
    struct stat status;
    int error = fstatat(directory, objectFile, &status, AT_SYMLINK_NOFOLLOW) ? errno : ownedObject(&status);
    if (!error && unlinkat(directory, objectFile, 0)) error = errno;
    close(directory);
    if (error) { errno = error; return -1; }
    return 0;
}

#ifndef LSB_NOTIFICATION_SHM_TESTING
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interpositions[] = {
    {(const void *)&LSBNotificationShmOpen, (const void *)&shm_open}, {(const void *)&LSBNotificationShmUnlink, (const void *)&shm_unlink}
};
#endif
