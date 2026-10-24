// Only the simulated thread-pointer register changes; these tests never mutate the host thread's real TSD base.
#define LSB_MAIN_THREAD_BOOTSTRAP_TESTING
#include "../LiveSimBooter/MainThreadBootstrap.cpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

static unsigned checks;
static uintptr_t fakeTSD, requestedTSD;
static unsigned reads, writes;
static bool ignoreWrite;
alignas(16384) static unsigned char hostStorage[16384];

static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

uintptr_t LSBTestReadTSDBase() { ++reads; return fakeTSD; }
void LSBTestSetTSDBase(uintptr_t base) {
    ++writes;
    requestedTSD = base;
    if (!ignoreWrite) fakeTSD = base;
}

static void resetHost() {
    memset(hostStorage, 0xab, sizeof(hostStorage));
    fakeTSD = reinterpret_cast<uintptr_t>(hostStorage + 224);
    auto slots = reinterpret_cast<uintptr_t *>(fakeTSD);
    slots[0] = reinterpret_cast<uintptr_t>(hostStorage);
    slots[1] = reinterpret_cast<uintptr_t>(hostStorage + 172);
    reads = writes = 0;
    ignoreWrite = false;
}

static void checkFreshStorage(const lsb::MainThreadBootstrap &state, uint64_t threadId) {
    auto slots = reinterpret_cast<uintptr_t *>(state.simulatorTSD);
    auto object = reinterpret_cast<unsigned char *>(slots[0]);
    check(slots[0] == state.simulatorTSD - 224, "simulator pthread self points to its own prefix");
    check((slots[0] & 16383) == 0, "main-thread allocation is aligned to 16 KiB");
    check(slots[1] == slots[0] + 172, "simulator errno points inside its own storage");
    check(*reinterpret_cast<int *>(slots[1]) == 0, "bootstrap errno starts at zero");
    check(*reinterpret_cast<uint64_t *>(object + 216) == threadId, "bootstrap preserves supplied kernel thread ID");
    bool prefixZero = true, slotsZero = true, tailZero = true;
    for (size_t i = 0; i != 216; ++i) if (object[i]) prefixZero = false;
    for (size_t i = 2; i != 768; ++i) if (slots[i]) slotsZero = false;
    for (size_t i = 224 + 768 * sizeof(uintptr_t); i != 16384; ++i) if (object[i]) tailZero = false;
    check(prefixZero, "host pthread prefix state is not inherited");
    check(slotsZero, "all remaining 766 TSD slots are fresh");
    check(tailZero, "full 16 KiB allocation is cleared");
    check(state.hostTSD == reinterpret_cast<uintptr_t>(hostStorage + 224), "original TSD is retained for restoration");
    check(state.simulatorTSD != state.hostTSD, "simulator storage is distinct from host storage");
}

int main() {
    resetHost();
    lsb::MainThreadBootstrap state = {0, 0};
    constexpr uint64_t threadId = 0x7392abc541;
    check(lsb::prepareMainThreadBootstrap(nullptr, threadId) == EINVAL, "null state is rejected");
    check(lsb::prepareMainThreadBootstrap(&state, 0) == EINVAL, "missing thread ID is rejected");
    check(reads == 0 && writes == 0, "invalid arguments do not access TSD");

    for (uintptr_t invalid : {uintptr_t(0), uintptr_t(16), reinterpret_cast<uintptr_t>(hostStorage + 225)}) {
        fakeTSD = invalid;
        check(lsb::prepareMainThreadBootstrap(&state, threadId) == ENOTSUP, "invalid current TSD address is rejected");
        check(state.hostTSD == 0 && state.simulatorTSD == 0, "failed preparation leaves result untouched");
    }
    resetHost();
    auto hostSlots = reinterpret_cast<uintptr_t *>(fakeTSD);
    hostSlots[0] += 16;
    check(lsb::prepareMainThreadBootstrap(&state, threadId) == ENOTSUP, "incompatible pthread-self layout is rejected");
    resetHost();
    hostSlots[1] += 4;
    check(lsb::prepareMainThreadBootstrap(&state, threadId) == ENOTSUP, "incompatible errno layout is rejected");

    resetHost();
    unsigned char originalHost[sizeof(hostStorage)];
    memcpy(originalHost, hostStorage, sizeof(hostStorage));
    check(lsb::prepareMainThreadBootstrap(&state, threadId) == 0, "valid ARM64 host layout prepares successfully");
    checkFreshStorage(state, threadId);
    check(writes == 0 && fakeTSD == state.hostTSD, "preparation does not switch the active thread pointer");
    check(memcmp(originalHost, hostStorage, sizeof(hostStorage)) == 0, "preparation does not change host pthread or TSD");

    auto object = reinterpret_cast<void *>(state.simulatorTSD - 224);
    memset(object, 0xcd, 16384);
    check(lsb::prepareMainThreadBootstrap(&state, threadId + 1) == 0, "repreparation clears all previous bootstrap state");
    checkFreshStorage(state, threadId + 1);

    check(lsb::setTSDBase(state.simulatorTSD), "successful switch is confirmed by readback");
    check(writes == 1 && requestedTSD == state.simulatorTSD && fakeTSD == state.simulatorTSD, "setter targets requested TSD");
    check(lsb::setTSDBase(state.hostTSD), "original host TSD can be restored");
    ignoreWrite = true;
    check(!lsb::setTSDBase(state.simulatorTSD), "unchanged register produces readback failure");
    check(fakeTSD == state.hostTSD, "failed simulated switch leaves original register intact");
    printf("Main-thread bootstrap: %u checks passed\n", checks);
}
