#include "DebuggerBridge.h"
#include "CompactImageInfo.h"
#include "LocalPOSIX.h"
#include "LocalRuntime.h"
#include "SimSyscallHelpers.h"

#include <cerrno>
#include <limits.h>
#include <sys/mman.h>
#include <sys/syscall.h>

namespace {

enum class BridgePhase { empty, prepared, begun, finished, cancelled };
struct DebuggerBridgeState {
    BridgePhase phase;
    dyld_all_image_infos *sim;
    dyld_all_image_infos *host;
    dyld_all_image_infos hostSnapshot;
    uintptr_t replacementBase;
    size_t replacementSize;
    bool canReplaceHost;
    bool published;
    dyld_image_info seedImages[2];
    dyld_image_info dyldImage;
    char appPath[PATH_MAX];
    char executablePath[PATH_MAX];
};
DebuggerBridgeState sBridge = {};

uintptr_t address(const void *pointer) { return reinterpret_cast<uintptr_t>(pointer); }

void copyInfo(dyld_all_image_infos *destination, const dyld_all_image_infos *source) {
    lsb::copy(destination, source, sizeof(*destination));
    destination->dyldAllImageInfosAddress = destination;
}

void notifyAdding(dyld_all_image_infos *info) {
    LSBDebuggerImageNotification(dyld_image_adding, info->infoArrayCount, info->infoArray);
}

} // namespace

namespace lsb {

int prepareDebuggerBridge(dyld_all_image_infos *sim, dyld_all_image_infos *host, size_t hostInfoSize, size_t pageSize,
                          const mach_header *appHeader, const char *appPath, const mach_header *executableHeader, const char *executablePath,
                          uintptr_t cacheBase, size_t cacheSize) {
    if (sBridge.phase != BridgePhase::empty) return EBUSY;
    if (!sim || !host || sim == host || !appHeader || !appPath || !executableHeader || !executablePath) return EINVAL;
    if (hostInfoSize < sizeof(*host) || pageSize < alignof(dyld_all_image_infos) || (pageSize & (pageSize - 1))) return EINVAL;
    const uintptr_t hostAddress = address(host), simAddress = address(sim), pageMask = pageSize - 1;
    if (hostAddress % alignof(dyld_all_image_infos) || simAddress % alignof(dyld_all_image_infos)) return EINVAL;
    if (hostInfoSize > UINTPTR_MAX - hostAddress || cacheSize > UINTPTR_MAX - cacheBase) return EOVERFLOW;
    const uintptr_t hostEnd = hostAddress + hostInfoSize;
    if (hostEnd > UINTPTR_MAX - pageMask || sizeof(*sim) > UINTPTR_MAX - simAddress) return EOVERFLOW;
    if (simAddress < hostEnd && simAddress + sizeof(*sim) > hostAddress) return EINVAL;
    if (cacheSize && simAddress < cacheBase + cacheSize && simAddress + sizeof(*sim) > cacheBase) return EINVAL;
    if (!sim->dyldImageLoadAddress || !sim->dyldPath) return EINVAL;
    if (!host->notification) return ENOTSUP;
    const uintptr_t replacementBase = hostAddress & ~pageMask, replacementEnd = (hostEnd + pageMask) & ~pageMask;
    const bool inCache = cacheSize && replacementBase >= cacheBase && replacementEnd <= cacheBase + cacheSize;
    if (inCache && simAddress < replacementEnd && simAddress + sizeof(*sim) > replacementBase) return EINVAL;
    if (lsb::copyString(sBridge.appPath, appPath, sizeof(sBridge.appPath)) >= sizeof(sBridge.appPath)) return ENAMETOOLONG;
    if (lsb::copyString(sBridge.executablePath, executablePath, sizeof(sBridge.executablePath)) >= sizeof(sBridge.executablePath)) return ENAMETOOLONG;

    sBridge.sim = sim;
    sBridge.host = host;
    lsb::copy(&sBridge.hostSnapshot, host, sizeof(*host));
    sBridge.replacementBase = replacementBase;
    sBridge.replacementSize = replacementEnd - replacementBase;
    sBridge.canReplaceHost = inCache;
    // dyld_sim inherits this list and omits its main executable from later load notifications, assuming the host already inserted it.
    sBridge.seedImages[0] = {appHeader, sBridge.appPath, 0};
    sBridge.seedImages[1] = {executableHeader, sBridge.executablePath, 0};
    sBridge.dyldImage = {sim->dyldImageLoadAddress, sim->dyldPath, 0};
    sim->infoArray = sBridge.seedImages;
    sim->infoArrayCount = 2;
    sim->initialImageCount = 2;
    sim->notification = &LSBDebuggerImageNotification;
    sim->dyldAllImageInfosAddress = sim;
    int error = lsb::publishCompactImageInfo(sim);
    if (error) return error;
    sBridge.phase = BridgePhase::prepared;
    return 0;
}

int beginDebuggerHandoff() {
    if (sBridge.phase != BridgePhase::prepared) return EINVAL;
    constexpr uintptr_t setDyldImages = 0xf;
    const auto result = lsb::syscall(SYS_proc_info, setDyldImages, lsb::getpid(), 0, 0, address(sBridge.sim), sizeof(*sBridge.sim));
    if (result.error && (result.error != EINVAL || !sBridge.canReplaceHost)) return result.error;
    sBridge.published = !result.error;
    sBridge.phase = BridgePhase::begun;
    if (!sBridge.published) sBridge.host->notification = &LSBDebuggerImageNotification;

    // LLDB watches the previous function. Mode 3 makes it reread TASK_DYLD_INFO and install a one-shot on our notification function.
    sBridge.hostSnapshot.notification(dyld_image_dyld_moved, 1, &sBridge.dyldImage);
    if (!sBridge.published) copyInfo(sBridge.host, sBridge.sim);
    notifyAdding(sBridge.published ? sBridge.sim : sBridge.host);
    return 0;
}

int finishDebuggerHandoff(bool cacheDetached) {
    if (sBridge.phase != BridgePhase::begun) return EINVAL;
    dyld_all_image_infos *active = sBridge.sim;
    if (!sBridge.published) {
        if (cacheDetached) {
            // Detachment replaces the entire shared region with inaccessible memory. Recreate only the registered metadata pages.
            void *mapping = lsb::mmap(reinterpret_cast<void *>(sBridge.replacementBase), sBridge.replacementSize,
                                      PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
            if (mapping == MAP_FAILED) return *lsb::errnoAddress();
            if (address(mapping) != sBridge.replacementBase) return EFAULT;
        }
        active = sBridge.host;
        copyInfo(active, sBridge.sim);
    }
    active->processDetachedFromSharedRegion = cacheDetached;
    LSBSetSimProcessInfo(active);
    sBridge.phase = BridgePhase::finished;
    notifyAdding(active);
    return 0;
}

void cancelDebuggerHandoff() {
    if (sBridge.phase != BridgePhase::begun) return;
    dyld_all_image_infos *registered = sBridge.published ? sBridge.sim : sBridge.host;
    copyInfo(registered, &sBridge.hostSnapshot);
    sBridge.phase = BridgePhase::cancelled;
    // The kernel pointer cannot be changed again. The saved host fields remain valid because this path precedes cache removal.
    const dyld_image_info hostDyld = {registered->dyldImageLoadAddress, registered->dyldPath, 0};
    LSBDebuggerImageNotification(dyld_image_dyld_moved, 1, &hostDyld);
    sBridge.hostSnapshot.notification(dyld_image_adding, registered->infoArrayCount, registered->infoArray);
}

} // namespace lsb
