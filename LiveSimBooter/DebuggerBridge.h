#pragma once

#include <cstddef>
#include <cstdint>
#include <mach-o/dyld_images.h>

namespace lsb {

// Prepare while host dyld and libSystem are mapped. All paths used after handoff are owned by the bridge or simulator metadata.
int prepareDebuggerBridge(dyld_all_image_infos *sim, dyld_all_image_infos *host, size_t hostInfoSize, size_t pageSize,
                          const mach_header *appHeader, const char *appPath, const mach_header *executableHeader, const char *executablePath,
                          uintptr_t cacheBase, size_t cacheSize);
int beginDebuggerHandoff();
// Call immediately after cache removal, before entering dyld_sim. An error after removal cannot restore the host runtime.
int finishDebuggerHandoff(bool cacheDetached);
// Only valid before cache removal, after restoring host TLS. Publication is irreversible, so restore contents at the registered address.
void cancelDebuggerHandoff();

} // namespace lsb
