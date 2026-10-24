/*
 * Copyright (c) 2004-2025 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
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

// Modified 2026-10-23: replace host-cache callbacks with app-local runtime, BSD syscall and Mach implementations.
// Helper-table ordering adapted from dyldMain.cpp at apple-oss-distributions/dyld fd8d0c4d52320ebf64db34f3cb280310d905c5ae.
#include "SimSyscallHelpers.h"
#include "LocalRuntime.h"
#include "LocalPOSIX.h"
#include "LocalMach.h"

#include <errno.h>
#include <limits.h>
#include <mach-o/dyld_images.h>

namespace {

dyld_all_image_infos sProcessInfo = {};
char sDyldPath[PATH_MAX] = {};
bool sProcessInfoInitialized = false;

bool compareAndSwapPointer(void *oldValue, void *newValue, void *volatile *address) {
    return __atomic_compare_exchange_n(address, &oldValue, newValue, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

void memoryBarrier() { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
dyld_all_image_infos *getProcessInfo() { return &sProcessInfo; }

// Keep ABI slots for disabled tracing and debugger notifications without depending on host libraries.
void imageNotification(dyld_image_mode, uint32_t, const dyld_image_info *) {}
void coreSymbolicationNotification(void *, uint64_t, const char *, const mach_header *) {}
void notifyMonitor() {}
void notifyImageChanges(bool, unsigned, const mach_header *[], const char *[]) {}
bool tracingEnabled(uint32_t) { return false; }
int trace(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) { return 0; }
uint64_t traceString(uint32_t, uint64_t, const char *) { return 0; }

const SimSyscallHelpers sHelpers = {
    18,
    &lsb::open,
    &lsb::close,
    &lsb::pread,
    &lsb::write,
    &lsb::mmap,
    &lsb::munmap,
    &lsb::madvise,
    &lsb::stat,
    &lsb::fcntl,
    &lsb::ioctl,
    &lsb::issetugid,
    &lsb::getcwd,
    &lsb::realpath,
    &lsb::vm_allocate,
    &lsb::vm_deallocate,
    &lsb::vm_protect,
    &lsb::vlog,
    &lsb::vlog,
    &lsb::pthread_mutex_lock,
    &lsb::pthread_mutex_unlock,
    &lsb::mach_thread_self,
    &lsb::mach_port_deallocate,
    &lsb::task_self_trap,
    &lsb::mach_timebase_info,
    &compareAndSwapPointer,
    &memoryBarrier,
    &getProcessInfo,
    &lsb::errnoAddress,
    &lsb::mach_absolute_time,
    &lsb::thread_switch,
    &lsb::opendir,
    &lsb::readdir_r,
    &lsb::closedir,
    &coreSymbolicationNotification,
    &coreSymbolicationNotification,
    &lsb::proc_regionfilename,
    &lsb::getpid,
    &lsb::mach_port_insert_right,
    &lsb::mach_port_allocate,
    &lsb::mach_msg,
    &lsb::abort_with_payload,
    &lsb::task_register_dyld_image_infos,
    &lsb::task_unregister_dyld_image_infos,
    &lsb::task_get_dyld_image_infos,
    &lsb::task_register_dyld_shared_cache_image_info,
    &lsb::task_register_dyld_set_dyld_state,
    &lsb::task_register_dyld_get_process_state,
    &lsb::task_info,
    &lsb::thread_info,
    &tracingEnabled,
    &trace,
    &traceString,
    &lsb::amfi_check_dyld_policy_self,
    &notifyMonitor,
    &notifyImageChanges,
    &lsb::mach_msg_destroy,
    &lsb::mach_port_construct,
    &lsb::mach_port_destruct,
    &lsb::fstat,
    &lsb::vm_copy,
    &lsb::task_dyld_process_info_notify_get,
    &lsb::fsgetpath,
    &lsb::getattrlistbulk,
    &lsb::getattrlist,
    &lsb::getfsstat,
    &notifyMonitor,
    &lsb::sysctlbyname,
};

} // namespace

const SimSyscallHelpers *LSBGetSimSyscallHelpers() { return &sHelpers; }

bool LSBInitializeSimProcessInfo(const mach_header *dyldHeader, const char *dyldPath) {
    if (sProcessInfoInitialized) { *lsb::errnoAddress() = EBUSY; return false; }
    if (!dyldHeader || !dyldPath) { *lsb::errnoAddress() = EINVAL; return false; }
    if (lsb::copyString(sDyldPath, dyldPath, sizeof(sDyldPath)) >= sizeof(sDyldPath)) {
        *lsb::errnoAddress() = ENAMETOOLONG;
        return false;
    }
    sProcessInfo.version = 17;
    sProcessInfo.notification = &imageNotification;
    sProcessInfo.processDetachedFromSharedRegion = true;
    sProcessInfo.dyldImageLoadAddress = dyldHeader;
    sProcessInfo.dyldVersion = "dyld_sim (LiveSimBooter)";
    sProcessInfo.dyldAllImageInfosAddress = &sProcessInfo;
    sProcessInfo.dyldPath = sDyldPath;
    sProcessInfo.platform = PLATFORM_IOSSIMULATOR;
    sProcessInfoInitialized = true;
    return true;
}
