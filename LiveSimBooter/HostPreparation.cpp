#include "HostPreparation.h"
#include "LocalPOSIX.h"

#include <cerrno>
#include <cstdio>
#include <dlfcn.h>
#include <pthread.h>

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
