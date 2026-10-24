// App-owned bootstrap storage for the ARM64 pthread ABI used by the selected simulator support libraries.
#include "MainThreadBootstrap.h"

#include <errno.h>
#include <stddef.h>

#if defined(LSB_MAIN_THREAD_BOOTSTRAP_TESTING)
uintptr_t LSBTestReadTSDBase();
void LSBTestSetTSDBase(uintptr_t base);
#elif !defined(__APPLE__) || !defined(__arm64__)
#error Main-thread bootstrap requires Darwin ARM64.
#endif

namespace lsb {
namespace {

// Selected ARM64 libpthread ABI: fixed 224-byte prefix, with 768 TSD slots in the macOS simulator support library.
// The iPhone host has fewer slots. Start fresh instead of cloning its smaller object or retaining host TLS values.
constexpr size_t kTSDOffset = 224;
constexpr size_t kErrnoOffset = 172;
constexpr size_t kThreadIdOffset = 216;
constexpr size_t kTSDSlotCount = 768;
constexpr size_t kStorageSize = 16384;

struct alignas(kStorageSize) BootstrapStorage {
    unsigned char prefix[kErrnoOffset];
    int error;
    unsigned char remainingPrefix[kThreadIdOffset - kErrnoOffset - sizeof(int)];
    uint64_t threadId;
    uintptr_t tsd[kTSDSlotCount];
    unsigned char padding[kStorageSize - kTSDOffset - kTSDSlotCount * sizeof(uintptr_t)];
};

static_assert(sizeof(uintptr_t) == 8 && sizeof(int) == 4, "Unexpected ARM64 data model");
static_assert(offsetof(BootstrapStorage, error) == kErrnoOffset, "Unexpected pthread errno offset");
static_assert(offsetof(BootstrapStorage, threadId) == kThreadIdOffset, "Unexpected pthread thread-ID offset");
static_assert(offsetof(BootstrapStorage, tsd) == kTSDOffset, "Unexpected pthread TSD offset");
static_assert(sizeof(BootstrapStorage) == kStorageSize, "Unexpected main-thread storage size");

BootstrapStorage sMainThread;

}

uintptr_t currentTSDBase() {
#if defined(LSB_MAIN_THREAD_BOOTSTRAP_TESTING)
    return LSBTestReadTSDBase();
#else
    uintptr_t base;
    __asm volatile("mrs %0, TPIDRRO_EL0" : "=r"(base) : : "memory");
    return base;
#endif
}

bool setTSDBase(uintptr_t base) {
#if defined(LSB_MAIN_THREAD_BOOTSTRAP_TESTING)
    LSBTestSetTSDBase(base);
#else
    // XNU platform syscall selector 2 updates the calling thread's TSD base; it does not return a BSD errno.
    register uintptr_t x0 __asm("x0") = base;
    register uintptr_t x3 __asm("x3") = 2;
    register uintptr_t x16 __asm("x16") = 0x80000000;
    __asm volatile("svc #0x80" : "+r"(x0), "+r"(x3), "+r"(x16) : : "cc", "memory");
#endif
    return currentTSDBase() == base;
}

int prepareMainThreadBootstrap(MainThreadBootstrap *state, uint64_t threadId) {
    if (!state || !threadId) return EINVAL;
    uintptr_t hostTSD = currentTSDBase();
    if (hostTSD < kTSDOffset || (hostTSD & 15)) return ENOTSUP;
    auto slots = reinterpret_cast<const uintptr_t *>(hostTSD);
    uintptr_t hostThread = hostTSD - kTSDOffset;
    if (slots[0] != hostThread || slots[1] != hostThread + kErrnoOffset) return ENOTSUP;

    // Do not retain host TLS keys or pthread pointers that may reference the shared cache. Libpthread finishes initialization.
    auto bytes = reinterpret_cast<volatile unsigned char *>(&sMainThread);
    for (size_t i = 0; i != sizeof(sMainThread); ++i) bytes[i] = 0;
    sMainThread.threadId = threadId;
    sMainThread.tsd[0] = reinterpret_cast<uintptr_t>(&sMainThread);
    sMainThread.tsd[1] = reinterpret_cast<uintptr_t>(&sMainThread.error);
    state->hostTSD = hostTSD;
    state->simulatorTSD = reinterpret_cast<uintptr_t>(&sMainThread.tsd[0]);
    return 0;
}

}
