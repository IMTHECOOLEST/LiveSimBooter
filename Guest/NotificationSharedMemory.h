#ifndef LSB_NOTIFICATION_SHARED_MEMORY_H
#define LSB_NOTIFICATION_SHARED_MEMORY_H

#include <sys/types.h>

#define LSB_NOTIFICATION_SHM_ENABLE_ENV "LSB_SIMULATE_NOTIFICATION_SHM"
#define LSB_NOTIFICATION_SHM_ROOT_ENV "SIMULATOR_SHARED_RESOURCES_DIRECTORY"

// Explicitly scoped simulator object; all unrelated names retain native POSIX shared-memory behavior.
// Enabled opens use an owned regular file in the canonical data directory's existing tmp subdirectory, with FD_CLOEXEC.
// Mode is required only with O_CREAT, matching the native variadic declaration. Errors use -1/errno.
int LSBNotificationShmOpen(const char *name, int flags, ...);
int LSBNotificationShmUnlink(const char *name);

#endif
