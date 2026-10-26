#ifndef LSB_GUEST_CODE_SIGNATURE_H
#define LSB_GUEST_CODE_SIGNATURE_H

enum { LSB_GUEST_CDHASH_BYTES = 20 };

#ifdef __cplusplus
extern "C" {
#endif
// Host-only reader. Hash the strongest supported embedded CodeDirectory, preserving the fd and its offset.
// Returns an errno value; hash is all-zero on failure. A successful result is the kernel's 20-byte CDHash representation.
// This does not authenticate CMS signatures or rehash code pages; the caller must supply its already authorized guest image.
// Thin arm64 only, 4/16 KiB signing pages, CodeDirectory versions 0x20000 through 0x20600, signature size at most 16 MiB.
int LSBReadMachOCodeDirectoryHash(int fd, unsigned char hash[LSB_GUEST_CDHASH_BYTES]);
#ifdef __cplusplus
}
#endif
#endif
