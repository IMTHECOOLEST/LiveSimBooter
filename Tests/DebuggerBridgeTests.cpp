#include "../LiveSimBooter/DebuggerBridge.cpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
unsigned checks;
alignas(16384) unsigned char hostPages[32768];
dyld_all_image_infos simulator;
dyld_all_image_infos *host, *registeredInfo, *activeInfo;
dyld_image_info originalImages[2];
mach_header appHeader = {}, simHeader = {}, hostDyldHeader = {}, executableHeader = {};
int syscallError, mapError, localError, compactError;
unsigned publishCalls, mapCalls, compactCalls;
bool wrongMapAddress;
struct Event { bool host; dyld_image_mode mode; uint32_t count; dyld_image_notifier registeredNotifier; };
std::vector<Event> events;

void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

void originalNotifier(dyld_image_mode mode, uint32_t count, const dyld_image_info *) {
    events.push_back({true, mode, count, registeredInfo->notification});
}

void reset(size_t offset = 64) {
    memset(&sBridge, 0, sizeof(sBridge));
    memset(&simulator, 0, sizeof(simulator));
    memset(hostPages, 0, sizeof(hostPages));
    host = reinterpret_cast<dyld_all_image_infos *>(hostPages + offset);
    host->version = 18;
    host->notification = &originalNotifier;
    host->infoArrayCount = 2;
    host->infoArray = originalImages;
    host->dyldImageLoadAddress = &hostDyldHeader;
    host->dyldAllImageInfosAddress = host;
    host->dyldPath = "/usr/lib/dyld";
    host->sharedCacheBaseAddress = address(hostPages);
    simulator.version = 17;
    simulator.notification = &LSBDebuggerImageNotification;
    simulator.dyldImageLoadAddress = &simHeader;
    simulator.dyldPath = "/Documents/RuntimeRoot/usr/lib/dyld_sim";
    simulator.dyldAllImageInfosAddress = &simulator;
    registeredInfo = host;
    activeInfo = &simulator;
    syscallError = mapError = localError = compactError = 0;
    publishCalls = mapCalls = compactCalls = 0;
    wrongMapAddress = false;
    events.clear();
}

int prepare(bool inCache = true, const char *path = "/Applications/LiveSimBooter.app/LiveSimBooter", const char *executablePath = "/Documents/RuntimeRoot/bin/df") {
    return lsb::prepareDebuggerBridge(&simulator, host, sizeof(*host), 16384, &appHeader, path, &executableHeader, executablePath,
                                      inCache ? address(hostPages) : 0, inCache ? sizeof(hostPages) : 0);
}

void checkRestored(dyld_all_image_infos *info) {
    check(info->dyldAllImageInfosAddress == info, "restored self pointer follows registered storage");
    check(info->notification == originalNotifier && info->dyldImageLoadAddress == &hostDyldHeader, "original host dyld and notifier restored");
    check(info->infoArray == originalImages && info->infoArrayCount == 2, "original host image array restored");
    check(info->sharedCacheBaseAddress == address(hostPages), "original shared-cache metadata restored");
}

void checkTransitionEvents() {
    check(events.size() == 2, "begin emits precisely the relocation and restart notifications");
    check(events[0].host && events[0].mode == dyld_image_dyld_moved && events[0].count == 1, "old notifier emits one dyld relocation");
    check(events[0].registeredNotifier == LSBDebuggerImageNotification, "relocation exposes the new breakpoint target");
    check(!events[1].host && events[1].mode == dyld_image_adding && events[1].count == 2, "new notifier starts with the resident host app and simulator executable");
}
} // namespace

extern "C" void LSBDebuggerImageNotification(dyld_image_mode mode, uint32_t count, const dyld_image_info *) {
    events.push_back({false, mode, count, registeredInfo->notification});
}

void LSBSetSimProcessInfo(dyld_all_image_infos *info) { activeInfo = info; }

namespace lsb {
int publishCompactImageInfo(dyld_all_image_infos *info, uint64_t state) {
    check(state == 0, "prepare infers the initial debugger state");
    ++compactCalls;
    check(info == &simulator && info->infoArrayCount == 2, "initial compact snapshot contains both executable images");
    return compactError;
}
void *copy(void *destination, const void *source, size_t size) { return memcpy(destination, source, size); }
size_t copyString(char *destination, const char *source, size_t capacity) { return strlcpy(destination, source, capacity); }
pid_t getpid() { return 932; }
int *errnoAddress() { return &localError; }

SyscallResult syscall(uintptr_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                      uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
    ++publishCalls;
    check(number == SYS_proc_info && a0 == 0xf && a1 == 932, "publication uses proc_info self-PID selector");
    check(!a2 && !a3 && !a6 && !a7 && a4 == address(&simulator) && a5 == sizeof(simulator), "publication passes exact private metadata range");
    if (!syscallError) registeredInfo = &simulator;
    return {static_cast<uintptr_t>(syscallError), syscallError};
}

void *mmap(void *location, size_t size, int protection, int flags, int fd, off_t offset) {
    ++mapCalls;
    check(address(location) == sBridge.replacementBase && size == sBridge.replacementSize, "replacement covers only the original metadata pages");
    check(protection == (PROT_READ | PROT_WRITE) && flags == (MAP_FIXED | MAP_ANON | MAP_PRIVATE) && fd == -1 && !offset,
          "replacement is fixed anonymous private writable memory");
    if (mapError) { localError = mapError; return MAP_FAILED; }
    if (wrongMapAddress) return hostPages + 16384;
    memset(location, 0, size);
    return location;
}
} // namespace lsb

int main() {
    reset();
    check(lsb::beginDebuggerHandoff() == EINVAL && lsb::finishDebuggerHandoff(true) == EINVAL, "unprepared transitions are rejected");
    check(lsb::prepareDebuggerBridge(nullptr, host, sizeof(*host), 16384, &appHeader, "app", &executableHeader, "df", 0, 0) == EINVAL, "null simulator rejected");
    check(lsb::prepareDebuggerBridge(&simulator, host, sizeof(*host) - 1, 16384, &appHeader, "app", &executableHeader, "df", 0, 0) == EINVAL,
          "truncated host metadata rejected");
    check(lsb::prepareDebuggerBridge(&simulator, host, sizeof(*host), 12345, &appHeader, "app", &executableHeader, "df", 0, 0) == EINVAL,
          "non-power-of-two page size rejected");
    check(lsb::prepareDebuggerBridge(&simulator, host, SIZE_MAX, 16384, &appHeader, "app", &executableHeader, "df", 0, 0) == EOVERFLOW,
          "host range overflow rejected");
    check(lsb::prepareDebuggerBridge(&simulator, host, sizeof(*host), 16384, &appHeader, "app", &executableHeader, "df", UINTPTR_MAX - 3, 16) == EOVERFLOW,
          "cache range overflow rejected");
    check(lsb::prepareDebuggerBridge(&simulator, host, sizeof(*host), 16384, &appHeader, "app", nullptr, "df", 0, 0) == EINVAL,
          "missing simulator executable header rejected");
    check(lsb::prepareDebuggerBridge(&simulator, host, sizeof(*host), 16384, &appHeader, "app", &executableHeader, nullptr, 0, 0) == EINVAL,
          "missing simulator executable path rejected");
    char overlong[PATH_MAX + 1];
    memset(overlong, 'a', sizeof(overlong) - 1);
    overlong[sizeof(overlong) - 1] = 0;
    check(prepare(true, overlong) == ENAMETOOLONG, "oversized app path rejected");
    check(prepare(true, "app", overlong) == ENAMETOOLONG, "oversized simulator executable path rejected");
    check(!publishCalls && !mapCalls && events.empty(), "preparation failures make no externally visible changes");

    compactError = ENOMEM;
    check(prepare() == ENOMEM && sBridge.phase == BridgePhase::empty, "compact snapshot failure aborts preparation before bridge is ready");
    check(!publishCalls && !mapCalls && events.empty(), "compact failure does not change debugger registration");
    compactError = 0;

    char appPath[] = "/app/LiveSimBooter";
    char executablePath[] = "/runtime/bin/df";
    check(prepare(true, appPath, executablePath) == 0, "valid preparation succeeds");
    appPath[1] = 'z';
    executablePath[1] = 'z';
    check(strcmp(simulator.infoArray[0].imageFilePath, "/app/LiveSimBooter") == 0, "app image path has owned storage");
    check(strcmp(simulator.infoArray[1].imageFilePath, "/runtime/bin/df") == 0, "simulator executable path has owned storage");
    check(simulator.infoArray[0].imageLoadAddress == &appHeader && simulator.infoArrayCount == 2, "simulator initial list includes both executable images");
    check(simulator.infoArray[1].imageLoadAddress == &executableHeader && simulator.initialImageCount == 2, "mapped simulator executable is seeded before dyld entry");
    check(simulator.dyldImageLoadAddress == &simHeader && simulator.dyldAllImageInfosAddress == &simulator, "dyld simulator and self pointers preserved");
    check(prepare() == EBUSY, "prepared bridge cannot be overwritten");
    check(lsb::beginDebuggerHandoff() == 0, "successful kernel publication starts handoff");
    checkTransitionEvents();
    check(lsb::beginDebuggerHandoff() == EINVAL && publishCalls == 1, "publication cannot be repeated");
    check(lsb::finishDebuggerHandoff(true) == 0, "published handoff finishes after detachment");
    check(!mapCalls && activeInfo == &simulator && registeredInfo == &simulator, "successful publication needs no old-address replacement");
    check(activeInfo->processDetachedFromSharedRegion, "published metadata records cache detachment");
    check(events.size() == 3 && !events.back().host && events.back().mode == dyld_image_adding, "finish refreshes stable image list");
    lsb::cancelDebuggerHandoff();
    check(events.size() == 3, "completed handoff cannot restore unloaded host pointers");

    reset(16384 - 16);
    check(prepare() == 0, "metadata straddling two pages prepares");
    check(sBridge.replacementSize == 32768, "replacement includes both intersecting pages");
    syscallError = EINVAL;
    check(lsb::beginDebuggerHandoff() == 0, "exhausted kernel relocation slot uses known-cache fallback");
    checkTransitionEvents();
    check(registeredInfo == host && host->dyldImageLoadAddress == &simHeader, "fallback changes contents without changing registered address");
    check(host->dyldAllImageInfosAddress == host && host->sharedCacheBaseAddress == 0, "temporary fallback snapshot has coherent self and no old cache");
    check(lsb::finishDebuggerHandoff(true) == 0, "fallback recreates metadata after detachment");
    check(mapCalls == 1 && activeInfo == host && host->dyldAllImageInfosAddress == host, "simulator uses rebuilt registered metadata");
    check(host->infoArray[0].imageFilePath == sBridge.appPath && host->notification == LSBDebuggerImageNotification, "rebuilt metadata retains app-owned pointers");

    reset();
    check(prepare() == 0, "fallback no-detachment setup prepares");
    syscallError = EINVAL;
    check(!lsb::beginDebuggerHandoff() && !lsb::finishDebuggerHandoff(false), "fallback supports already-retained cache");
    check(!mapCalls && activeInfo == host && !host->processDetachedFromSharedRegion, "retained-cache completion never overwrites mapping");

    for (int error : {EACCES, EPERM, ENOMEM}) {
        reset();
        check(!prepare(), "error-path setup prepares");
        syscallError = error;
        check(lsb::beginDebuggerHandoff() == error, "unrelated syscall errors are propagated");
        checkRestored(host);
        check(events.empty() && !mapCalls, "unrelated errors do not notify or map");
    }
    reset();
    check(!prepare(false), "host metadata outside cache permits publication attempt");
    syscallError = EINVAL;
    check(lsb::beginDebuggerHandoff() == EINVAL && events.empty(), "fallback refuses metadata outside known cache range");
    checkRestored(host);
    reset();
    check(!prepare(false) && !lsb::beginDebuggerHandoff() && !lsb::finishDebuggerHandoff(true), "publication works with host metadata outside cache");

    for (int error : {0, EINVAL}) {
        reset();
        check(!prepare(), "cancel-path setup prepares");
        syscallError = error;
        check(!lsb::beginDebuggerHandoff(), "cancel-path handoff begins");
        lsb::cancelDebuggerHandoff();
        checkRestored(registeredInfo);
        check(publishCalls == 1 && !mapCalls, "cancel restores metadata without attempting forbidden kernel rollback");
        check(events.size() == 4 && !events[2].host && events[2].mode == dyld_image_dyld_moved, "cancel announces relocation through current notifier");
        check(events[2].registeredNotifier == originalNotifier && events[3].host && events[3].mode == dyld_image_adding,
              "cancel reinstalls original notifier and host image list");
        check(lsb::finishDebuggerHandoff(true) == EINVAL, "cancelled handoff cannot finish");
    }
    reset();
    check(!prepare(), "mapping-error setup prepares");
    syscallError = EINVAL;
    check(!lsb::beginDebuggerHandoff(), "mapping-error handoff begins");
    mapError = ENOMEM;
    check(lsb::finishDebuggerHandoff(true) == ENOMEM && activeInfo == &simulator, "fixed mapping failure propagates before changing simulator pointer");
    printf("Debugger bridge: %u checks passed\n", checks);
}
