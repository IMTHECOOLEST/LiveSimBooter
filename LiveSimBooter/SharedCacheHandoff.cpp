/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

// Modified 2026-10-23: adapt shared-region removal to executable-local arm64 syscalls and simulator handoff.
// Modified 2026-10-24: switch to app-owned main-thread storage before cache removal and restore host TLS on failure.
// Modified 2026-10-24: invoke the prepared simulator main wrapper and preserve its return status without host cleanup.
// Modified 2026-10-24: transfer debugger notifications and preserve registered image metadata across cache removal.
// Shared-region removal follows Apple's deallocateExistingSharedCache() at fd8d0c4d52320ebf64db34f3cb280310d905c5ae.
// https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/SharedCacheRuntime.cpp#L1230-L1243
#include "SharedCacheHandoff.h"
#include "DebuggerBridge.h"

#include <errno.h>
#include <sys/syscall.h>

#if defined(LSB_SHARED_CACHE_HANDOFF_TESTING)
// Compile-time substitutes: the host tests must never detach their real shared region or terminate the test process.
int LSBTestSharedRegionCheck(uint64_t *address);
[[noreturn]] void LSBTestExit(int status);
static inline int sharedRegionCheck(uint64_t *address) { return LSBTestSharedRegionCheck(address); }
[[noreturn]] static inline void exitProcess(int status) { LSBTestExit(status); }
#else
#if !defined(__arm64__)
#error Shared-cache handoff requires Darwin arm64.
#endif

static_assert(SYS_shared_region_check_np == 294 && SYS_exit == 1, "Unexpected Darwin syscall numbers");

__attribute__((always_inline, no_stack_protector)) static inline int sharedRegionCheck(uint64_t *address) {
    register uintptr_t result __asm("x0") = reinterpret_cast<uintptr_t>(address);
    register uintptr_t number __asm("x16") = SYS_shared_region_check_np;
    // Darwin reports a positive errno in x0 with carry set. Avoid libc's errno accessor after removing the cache.
    __asm volatile("svc #0x80\n\tcsel %0, %0, xzr, cs" : "+r"(result), "+r"(number) : : "x1", "cc", "memory");
    return static_cast<int>(result);
}

[[noreturn]] __attribute__((always_inline, no_stack_protector)) static inline void exitProcess(int status) {
    for (;;) {
        register uintptr_t result __asm("x0") = static_cast<uintptr_t>(status);
        register uintptr_t number __asm("x16") = SYS_exit;
        __asm volatile("svc #0x80" : "+r"(result), "+r"(number) : : "x1", "cc", "memory");
    }
}
#endif

__attribute__((noinline, no_stack_protector)) int LSBDetachHostCacheAndEnter(const LSBDyldSimLaunch *launch) {
    if (!launch || !launch->thread.hostTSD || !launch->thread.simulatorTSD || lsb::currentTSDBase() != launch->thread.hostTSD) return EINVAL;
    uint64_t existingCacheAddress = 0;
    int error = sharedRegionCheck(&existingCacheAddress);
    if (error && error != EINVAL && error != ENOMEM) return error;
    bool hasCache = !error;

    error = lsb::beginDebuggerHandoff();
    if (error) return error;

    // From this switch onward, even a mapped host libSystem cannot safely use our fresh pthread state.
    if (!lsb::setTSDBase(launch->thread.simulatorTSD)) {
        if (!lsb::setTSDBase(launch->thread.hostTSD)) exitProcess(127);
        lsb::cancelDebuggerHandoff();
        return EIO;
    }
    if (hasCache) {
        error = sharedRegionCheck(nullptr);
        if (error) {
            if (!lsb::setTSDBase(launch->thread.hostTSD)) exitProcess(127);
            lsb::cancelDebuggerHandoff();
            return error;
        }
    }

    if (lsb::finishDebuggerHandoff(hasCache)) exitProcess(126);

    // All instructions in this function live in the executable. No host library may be called after the detach succeeds.
    uintptr_t startGlue = 0;
    LSBDyldSimLaunch::Main main = launch->entry(launch->argc, launch->argv, launch->envp, launch->apple, launch->executable,
                                              launch->dyld, launch->slide, launch->helpers, &startGlue);
    if (!main) exitProcess(1);
    // This loader accepts LC_MAIN executables. A legacy crt1 entry (startGlue == 0) requires a different stack ABI.
    if (startGlue != 1) exitProcess(126);
    int status = main(launch->argc, launch->argv, launch->envp, launch->apple);
    // start_sim normally calls simulator exit(). If it returns, never enter unmapped host libc cleanup.
    exitProcess(status);
}
