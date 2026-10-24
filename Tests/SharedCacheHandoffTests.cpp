// All shared-region syscalls, TSD switches and exits are mocked; no real host cache or thread state is changed.
#define LSB_SHARED_CACHE_HANDOFF_TESTING
#include "../LiveSimBooter/SharedCacheHandoff.cpp"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <setjmp.h>
#include <vector>

static unsigned checks = 0;
static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

enum class Event { ReadTSD, Query, Switch, Detach, Restore, Entry, Main, Exit };
static std::vector<Event> events;
static constexpr uintptr_t hostTSD = 0x100020e0, simulatorTSD = 0x140000e0;
static uintptr_t currentTSD;
static jmp_buf exitJump;
static int queryError, detachError, queryCalls, detachCalls, switchCalls, restoreCalls, entryCalls, mainCalls, mainStatus, exitStatus;
static uintptr_t returnedStartGlue;
static bool returnMain, failSwitch, failRestore;
static const LSBDyldSimLaunch *expected;

static void expectEvents(std::initializer_list<Event> sequence) {
    check(events.size() == sequence.size(), "expected operation count");
    size_t index = 0;
    for (Event event : sequence) check(events[index++] == event, "TSD, cache and entry operations happen in safe order");
}

namespace lsb {
uintptr_t currentTSDBase() {
    events.push_back(Event::ReadTSD);
    return currentTSD;
}

bool setTSDBase(uintptr_t address) {
    if (address == simulatorTSD) {
        events.push_back(Event::Switch);
        ++switchCalls;
        check(queryCalls == 1 && detachCalls == 0 && entryCalls == 0, "TLS switches after query and before detach or entry");
        check(currentTSD == hostTSD, "simulator TLS switch starts from host TLS");
        // Even a failed switch may have changed kernel state; callers must explicitly restore host TLS.
        currentTSD = simulatorTSD;
        return !failSwitch;
    }
    check(address == hostTSD, "only the prepared simulator and original host TSD bases may be installed");
    events.push_back(Event::Restore);
    ++restoreCalls;
    check(switchCalls == 1 && entryCalls == 0, "rollback happens only after a switch and before simulator entry");
    if (!failRestore) currentTSD = hostTSD;
    return !failRestore;
}
}

int LSBTestSharedRegionCheck(uint64_t *address) {
    if (address) {
        events.push_back(Event::Query);
        ++queryCalls;
        check(currentTSD == hostTSD, "cache query uses original host TLS");
        check(switchCalls == 0 && detachCalls == 0, "query precedes TLS switch and cache removal");
        check(*address == 0, "query address storage is initialized");
        *address = 0x180000000;
        return queryError;
    }
    events.push_back(Event::Detach);
    ++detachCalls;
    check(currentTSD == simulatorTSD && switchCalls == 1, "cache removal requires simulator TLS already installed");
    check(!failSwitch && queryError == 0, "failed switch or query never removes the cache");
    return detachError;
}

[[noreturn]] void LSBTestExit(int status) {
    events.push_back(Event::Exit);
    if (failRestore) {
        check(status == 127 && restoreCalls == 1 && entryCalls == 0, "failed host TLS restoration exits before simulator entry");
    } else {
        check(entryCalls == 1 && currentTSD == simulatorTSD, "normal raw exit follows entry using simulator TLS");
    }
    exitStatus = status;
    longjmp(exitJump, 1);
}

static int fakeMain(int argc, const char *const argv[], const char *const envp[], const char *const apple[]) {
    events.push_back(Event::Main);
    ++mainCalls;
    check(entryCalls == 1 && returnMain && returnedStartGlue == 1, "main is invoked once after a valid LC_MAIN prepare result");
    check(currentTSD == simulatorTSD && restoreCalls == 0, "returned main keeps simulator TLS and never restores host TLS");
    check(queryCalls == 1 && detachCalls == (queryError ? 0 : 1), "returned main runs after completed cache handling");
    check(argc == expected->argc && argv == expected->argv && envp == expected->envp && apple == expected->apple,
          "returned main receives the same argument vectors as prepare");
    return mainStatus;
}

static LSBDyldSimLaunch::Main fakeEntry(int argc, const char *const argv[], const char *const envp[], const char *const apple[],
                                      const mach_header *executable, const mach_header *dyld, uintptr_t slide,
                                      const SimSyscallHelpers *helpers, uintptr_t *startGlue) {
    events.push_back(Event::Entry);
    ++entryCalls;
    check(currentTSD == simulatorTSD && switchCalls == 1 && restoreCalls == 0, "simulator entry uses fresh TLS without rollback");
    check(queryCalls == 1 && detachCalls == (queryError ? 0 : 1), "entry follows completed cache handling");
    check(!detachError && !failSwitch, "failed detach or TLS switch never enters dyld_sim");
    check(argc == expected->argc && argv == expected->argv && envp == expected->envp && apple == expected->apple,
          "entry receives original argument vectors");
    check(executable == expected->executable && dyld == expected->dyld && slide == expected->slide && helpers == expected->helpers,
          "entry receives executable, dyld and helpers unchanged");
    check(startGlue && *startGlue == 0, "entry receives initialized start glue storage");
    *startGlue = returnedStartGlue;
    return returnMain ? fakeMain : nullptr;
}

static void reset(int query = 0, int detach = 0, bool hasMain = true) {
    events.clear();
    currentTSD = hostTSD;
    queryError = query;
    detachError = detach;
    returnMain = hasMain;
    mainStatus = 0;
    returnedStartGlue = 1;
    failSwitch = failRestore = false;
    queryCalls = detachCalls = switchCalls = restoreCalls = entryCalls = mainCalls = 0;
    exitStatus = -1;
}

static void expectExit(const LSBDyldSimLaunch &launch, int status, int entries = 1, int mains = 1) {
    expected = &launch;
    if (setjmp(exitJump) == 0) {
        LSBDetachHostCacheAndEnter(&launch);
        check(false, "successful entry or failed TLS rollback must never return to host code");
    }
    check(exitStatus == status, "expected raw exit status");
    check(entryCalls == entries && mainCalls == mains, "expected prepare and returned main invocation counts");
}

static void expectReturn(const LSBDyldSimLaunch *launch, int error) {
    check(LSBDetachHostCacheAndEnter(launch) == error, "handoff returns the expected error");
    check(currentTSD == hostTSD, "return to host code requires original TLS restored");
    check(entryCalls == 0 && mainCalls == 0 && exitStatus == -1, "recoverable failure neither enters simulator nor exits");
}

int main() {
    const char *argv[] = {"/RuntimeRoot/bin/df", "-h", nullptr};
    const char *envp[] = {"DYLD_ROOT_PATH=/RuntimeRoot", nullptr};
    const char *apple[] = {"executable_path=/RuntimeRoot/bin/df", nullptr};
    LSBDyldSimLaunch launch = {fakeEntry, 2, argv, envp, apple, reinterpret_cast<const mach_header *>(0x100000000),
                               reinterpret_cast<const mach_header *>(0x120000000), 0x20000000,
                               reinterpret_cast<const SimSyscallHelpers *>(0x130000000), {hostTSD, simulatorTSD}};
    for (int status : {0, 42, 127}) {
        reset();
        mainStatus = status;
        expectExit(launch, status);
        expectEvents({Event::ReadTSD, Event::Query, Event::Switch, Event::Detach, Event::Entry, Event::Main, Event::Exit});
    }
    for (uintptr_t startGlue : {uintptr_t(0), uintptr_t(1), uintptr_t(0x12345678)}) {
        reset(0, 0, false);
        returnedStartGlue = startGlue;
        expectExit(launch, 1, 1, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::Switch, Event::Detach, Event::Entry, Event::Exit});
    }
    for (uintptr_t startGlue : {uintptr_t(0), uintptr_t(0x12345678)}) {
        reset();
        returnedStartGlue = startGlue;
        expectExit(launch, 126, 1, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::Switch, Event::Detach, Event::Entry, Event::Exit});
    }
    for (int noCache : {EINVAL, ENOMEM}) {
        reset(noCache);
        expectExit(launch, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::Switch, Event::Entry, Event::Main, Event::Exit});
    }
    for (int error : {EIO, EPERM}) {
        reset(error);
        expectReturn(&launch, error);
        expectEvents({Event::ReadTSD, Event::Query});
    }

    reset();
    expectReturn(nullptr, EINVAL);
    expectEvents({});
    for (bool missingHost : {true, false}) {
        reset();
        LSBDyldSimLaunch invalid = launch;
        if (missingHost) invalid.thread.hostTSD = 0;
        else invalid.thread.simulatorTSD = 0;
        expectReturn(&invalid, EINVAL);
        expectEvents({});
    }
    reset();
    LSBDyldSimLaunch mismatch = launch;
    mismatch.thread.hostTSD += 16;
    expectReturn(&mismatch, EINVAL);
    expectEvents({Event::ReadTSD});

    for (int query : {0, EINVAL, ENOMEM}) {
        reset(query);
        failSwitch = true;
        expectReturn(&launch, EIO);
        expectEvents({Event::ReadTSD, Event::Query, Event::Switch, Event::Restore});

        reset(query);
        failSwitch = failRestore = true;
        expectExit(launch, 127, 0, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::Switch, Event::Restore, Event::Exit});
    }
    for (int error : {EPERM, EIO}) {
        reset(0, error);
        expectReturn(&launch, error);
        expectEvents({Event::ReadTSD, Event::Query, Event::Switch, Event::Detach, Event::Restore});

        reset(0, error);
        failRestore = true;
        expectExit(launch, 127, 0, 0);
        expectEvents({Event::ReadTSD, Event::Query, Event::Switch, Event::Detach, Event::Restore, Event::Exit});
    }
    printf("Shared cache handoff: %u checks passed\n", checks);
}
