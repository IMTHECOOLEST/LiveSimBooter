#include "HostPreparation.h"
#include "DebuggerBridge.h"
#include "LocalPOSIX.h"
#include "SimSyscallHelpers.h"

#include <cerrno>
#include <cstdio>
#include <dlfcn.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <unistd.h>

int LSBPrepareHostDebugger(const mach_header *executable, const char *executablePath) {
    task_dyld_info_data_t info = {};
    mach_msg_type_number_t count = TASK_DYLD_INFO_COUNT;
    kern_return_t result = task_info(mach_task_self(), TASK_DYLD_INFO, reinterpret_cast<task_info_t>(&info), &count);
    if (result != KERN_SUCCESS || info.all_image_info_format != TASK_DYLD_ALL_IMAGE_INFO_64) return ENOTSUP;
    char path[PATH_MAX];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size)) return ENAMETOOLONG;
    using CacheRange = const void *(*)(size_t *);
    auto cacheRange = reinterpret_cast<CacheRange>(dlsym(RTLD_DEFAULT, "_dyld_get_shared_cache_range"));
    size_t cacheSize = 0;
    const void *cacheBase = cacheRange ? cacheRange(&cacheSize) : nullptr;
    return lsb::prepareDebuggerBridge(LSBGetSimSyscallHelpers()->getProcessInfo(),
                                     reinterpret_cast<dyld_all_image_infos *>(info.all_image_info_addr), info.all_image_info_size,
                                     static_cast<size_t>(getpagesize()), _dyld_get_image_header(0), path, executable, executablePath,
                                     reinterpret_cast<uintptr_t>(cacheBase), cacheSize);
}

int LSBPrepareHostMainThread(lsb::MainThreadBootstrap *bootstrap) {
    if (!pthread_main_np()) return ENOTSUP;
    uint64_t threadId = 0;
    int error = pthread_threadid_np(nullptr, &threadId);
    return error ? error : lsb::prepareMainThreadBootstrap(bootstrap, threadId);
}

void LSBPrepareHostPolicy() {
    // The pinned dyld simulator asks amfiFlags() with AMFI_DYLD_INPUT_PROC_IN_SIMULATOR (1), irrespective of executable flags.
    // AMFI's private kernel message ABI is not assumed here. Capture the real answer while its host wrapper is still mapped.
    using Query = int (*)(uint64_t, uint64_t *);
    auto query = reinterpret_cast<Query>(dlsym(RTLD_DEFAULT, "amfi_check_dyld_policy_self"));
    uint64_t output = 0;
    int result = -1, error = ENOSYS;
    if (query) {
        result = query(1, &output);
        error = result ? errno : 0;
    }
    lsb::setAMFIPolicyResult(1, result, result ? 0 : output, error);
    if (result) fprintf(stderr, "[dyld_sim] AMFI policy query unavailable or failed (%d); no policy permissions will be invented.\n", error);
}
