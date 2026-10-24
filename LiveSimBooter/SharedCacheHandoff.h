#pragma once

#include "MainThreadBootstrap.h"
#include <stdint.h>

struct mach_header;
struct SimSyscallHelpers;

struct LSBDyldSimLaunch {
    using Main = int (*)(int, const char *const[], const char *const[], const char *const[]);
    using Entry = Main (*)(int, const char *const[], const char *const[], const char *const[], const mach_header *, const mach_header *,
                          uintptr_t, const SimSyscallHelpers *, uintptr_t *);

    Entry entry;
    int argc;
    const char *const *argv;
    const char *const *envp;
    const char *const *apple;
    const mach_header *executable;
    const mach_header *dyld;
    uintptr_t slide;
    const SimSyscallHelpers *helpers;
    lsb::MainThreadBootstrap thread;
};

// Returns an errno before entry with host TLS restored. Successful handoff never returns to host code.
int LSBDetachHostCacheAndEnter(const LSBDyldSimLaunch *launch);
