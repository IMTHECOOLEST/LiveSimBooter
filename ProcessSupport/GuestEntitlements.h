#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LSB_GUEST_ENTITLEMENTS_DER_MAGIC UINT32_C(0xfade7172)
#define LSB_GUEST_ENTITLEMENTS_XML_MAGIC UINT32_C(0xfade7171)
#define LSB_GUEST_ENTITLEMENTS_MAX_BLOB_SIZE (256u * 1024u)
#define LSB_GUEST_ENTITLEMENTS_MAX_DER_SIZE (LSB_GUEST_ENTITLEMENTS_MAX_BLOB_SIZE - 8u)
#define LSB_GUEST_ENTITLEMENTS_MAX_XML_BLOB_SIZE (1024u * 1024u)

// Transport framing only. The host separately validates the original DER before publishing an immutable snapshot.
static inline bool LSBGuestEntitlementsHasValidBlobHeaderForOperation(const void *bytes, size_t size, unsigned int operation) {
    size_t limit;
    uint32_t expectedMagic;
    if (operation == 16) { limit = LSB_GUEST_ENTITLEMENTS_MAX_BLOB_SIZE; expectedMagic = LSB_GUEST_ENTITLEMENTS_DER_MAGIC; }
    else if (operation == 7) { limit = LSB_GUEST_ENTITLEMENTS_MAX_XML_BLOB_SIZE; expectedMagic = LSB_GUEST_ENTITLEMENTS_XML_MAGIC; }
    else return false;
    if (!bytes || size <= 8 || size > limit) return false;
    const unsigned char *p = bytes;
    uint32_t magic = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    uint32_t length = (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7];
    return magic == expectedMagic && length == size;
}

static inline bool LSBGuestEntitlementsHasValidBlobHeader(const void *bytes, size_t size) {
    return LSBGuestEntitlementsHasValidBlobHeaderForOperation(bytes, size, 16);
}

#ifdef __cplusplus
extern "C" {
#endif

// Validate a canonical, version-1 DER entitlement dictionary (the iOS 15+ format); zero or an errno-style error.
int LSBValidateGuestEntitlementsDER(const void *bytes, size_t size);

// Read __TEXT,__ents_der from a regular thin arm64 Mach-O using pread. The caller chooses/opens the trusted executable.
// Success returns malloc-owned bytes (free with free) including the 8-byte big-endian code-signing DER blob header.
// The fd stays open and its seek position is unchanged. On error, outputs are NULL/0; absent DER returns ENOENT.
int LSBReadMachODEREntitlements(int fd, unsigned char **bytes, size_t *size);

#ifdef __cplusplus
}
#endif
