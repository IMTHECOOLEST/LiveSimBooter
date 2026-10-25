#ifndef LSB_EXECUTABLE_ACCESS_H
#define LSB_EXECUTABLE_ACCESS_H

// Preserve native access checks, with a constrained executable preflight after sandbox EPERM under DYLD_ROOT_PATH.
// faccessat fallback supports absolute paths and AT_FDCWD-relative paths with flags 0 or AT_EACCESS only.
// This checks readable executable metadata, not code signatures; the broker and loader validate the eventual launch.
int LSBAccess(const char *path, int mode);
int LSBFAccessAt(int directory, const char *path, int mode, int flags);

#endif
