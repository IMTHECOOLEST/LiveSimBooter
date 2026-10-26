#pragma once

#include <cstddef>
#include <cstdint>
#include <mach-o/dyld_images.h>

namespace lsb {

constexpr size_t kCompactImageInfoMaxImages = 2048;

// Select the reader-compatible archive format from native dyld metadata while the host cache is still mapped.
// Only the encoding choice is retained. Unknown formats return ENOTSUP; malformed archives return EBADMSG.
// Must succeed before first publication; later calls return EBUSY. Native metadata is never modified.
int configureCompactImageInfo(const void *nativeArchive, size_t nativeArchiveSize);

// Publish an Apple Archive process.plist snapshot for the native debugger's dyld introspection. The legacy array remains owned by dyld_sim.
// At most kCompactImageInfoMaxImages distinct images, including dyld, are supported. Calls serialize through an app-local lock.
// Two app-owned buffers retain the current and previous snapshots; no pointers refer into the host cache.
// This serves LLDB while the process is stopped. Readers spanning two later publications need their own retry protocol.
// A nonzero initialization state (0x10, 0x30, 0x40 or 0x50) advances the reported state on successful publication.
// Successful publication also advances infoArrayChangeTimestamp; invalid input preserves all published metadata.
int publishCompactImageInfo(dyld_all_image_infos *info, uint64_t state = 0);

} // namespace lsb
