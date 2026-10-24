#pragma once

#include <cstdint>
#include <mach-o/dyld_images.h>

namespace lsb {

// Publish an Apple Archive process.plist snapshot for modern dyld introspection. The legacy array remains owned by dyld_sim.
// At most 1024 distinct images, including dyld, are supported. Calls serialize through an app-local lock.
// Two app-owned buffers retain the current and previous snapshots; no pointers refer into the host cache.
// This serves LLDB while the process is stopped. Readers spanning two later publications need their own retry protocol.
// A nonzero initialization state (0x10, 0x30, 0x40 or 0x50) advances the reported state on successful publication.
// Successful publication also advances infoArrayChangeTimestamp; invalid input preserves all published metadata.
int publishCompactImageInfo(dyld_all_image_infos *info, uint64_t state = 0);

} // namespace lsb
