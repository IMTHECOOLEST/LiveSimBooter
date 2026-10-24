#include "../LiveSimBooter/SimSyscallHelpers.h"
#include "../LiveSimBooter/LocalPOSIX.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mach-o/dyld_images.h>

int main() {
    const SimSyscallHelpers *helpers = LSBGetSimSyscallHelpers();
    assert(helpers->version == 18);
    Dl_info executable = {};
    assert(dladdr(reinterpret_cast<const void *>(&LSBGetSimSyscallHelpers), &executable));
    for (size_t slot = 1; slot < sizeof(*helpers) / sizeof(uintptr_t); ++slot) {
        uintptr_t callback;
        std::memcpy(&callback, reinterpret_cast<const char *>(helpers) + slot * sizeof(callback), sizeof(callback));
        Dl_info info = {};
        assert(callback && dladdr(reinterpret_cast<const void *>(callback), &info));
        assert(info.dli_fbase == executable.dli_fbase);
    }
    assert(helpers->getpid() == getpid());
    assert(helpers->task_self_trap() == mach_task_self());
    assert(helpers->errnoAddress() != &errno);
    assert(helpers->close(-1) == -1 && *helpers->errnoAddress() == EBADF);

    mach_header header = {};
    assert(!LSBInitializeSimProcessInfo(nullptr, "dyld_sim") && *helpers->errnoAddress() == EINVAL);
    char path[] = "/app/Documents/RuntimeRoot/usr/lib/dyld_sim";
    assert(LSBInitializeSimProcessInfo(&header, path));
    auto info = helpers->getProcessInfo();
    assert(info->dyldImageLoadAddress == &header && info->processDetachedFromSharedRegion);
    assert(info->dyldAllImageInfosAddress == info && info->platform == PLATFORM_IOSSIMULATOR);
    assert(info->dyldPath != path && std::strcmp(info->dyldPath, path) == 0);
    Dl_info notification = {};
    assert(dladdr(reinterpret_cast<const void *>(info->notification), &notification));
    assert(notification.dli_fbase == executable.dli_fbase);
    assert(!LSBInitializeSimProcessInfo(&header, path) && *helpers->errnoAddress() == EBUSY);

    uint64_t policy = 0;
    lsb::setAMFIPolicyResult(1, 0, 0x42, 0);
    assert(helpers->amfi_check_dyld_policy_self(1, &policy) == 0 && policy == 0x42);
    assert(helpers->amfi_check_dyld_policy_self(2, &policy) == -1 && policy == 0 && *helpers->errnoAddress() == ENOSYS);
    std::puts("Helper table integration: all 67 callbacks reside in the executable; safe calls passed");
}
