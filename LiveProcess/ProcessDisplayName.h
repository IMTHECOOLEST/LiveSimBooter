#ifndef LSB_PROCESS_DISPLAY_NAME_H
#define LSB_PROCESS_DISPLAY_NAME_H

#include <stddef.h>

// Native diagnostic metadata only. Neither the guest's arguments nor its executable identity is changed.
// argv0Capacity is the original writable argv[0] string size including its terminator, or zero to leave it alone.
// The kernel long name is capped at 32 bytes. Returns an errno value without changing errno.
int LSBSetProcessDisplayName(const char *path, char *nativeArgv0, size_t argv0Capacity);

#endif
