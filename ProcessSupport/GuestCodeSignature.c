#include "GuestCodeSignature.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/loader.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MaximumCommands = 1024 * 1024, MaximumSignature = 16 * 1024 * 1024, MaximumBlobs = 1024 };
enum { CodeDirectoryMagic = 0xfade0c02, SuperBlobMagic = 0xfade0cc0, AlternateSlot = 0x1000, AlternateLimit = 0x1005 };

// On-disk big-endian fields; versioned suffixes are read only when that version declares them.
typedef struct __attribute__((packed)) {
    uint32_t magic, length, version, flags, hashOffset, identifierOffset, specialSlots, codeSlots, codeLimit;
    uint8_t hashSize, hashType, platform, pageShift;
    uint32_t spare2, scatterOffset, teamOffset, spare3;
    uint64_t codeLimit64, executableBase, executableLimit, executableFlags;
    uint32_t runtime, preEncryptOffset;
    uint8_t linkageHashType, linkageApplicationType;
    uint16_t linkageApplicationSubtype;
    uint32_t linkageOffset, linkageSize;
} CodeDirectory;
typedef struct { uint32_t magic, length; } BlobHeader;
typedef struct { uint32_t magic, length, count; } SuperBlob;
typedef struct { uint32_t slot, offset; } BlobIndex;
typedef struct __attribute__((packed)) { uint32_t count, base; uint64_t targetOffset, spare; } Scatter;
_Static_assert(offsetof(CodeDirectory, scatterOffset) == 44, "CodeDirectory base ABI");
_Static_assert(offsetof(CodeDirectory, codeLimit64) == 56, "CodeDirectory 64-bit limit ABI");
_Static_assert(offsetof(CodeDirectory, runtime) == 88, "CodeDirectory runtime ABI");
_Static_assert(offsetof(CodeDirectory, linkageOffset) == 100 && sizeof(CodeDirectory) == 108, "CodeDirectory linkage ABI");
_Static_assert(sizeof(BlobHeader) == 8 && sizeof(SuperBlob) == 12 && sizeof(BlobIndex) == 8 && sizeof(Scatter) == 24, "Signature ABI");

static uint32_t big32(uint32_t value) { return OSSwapBigToHostInt32(value); }
static uint64_t big64(uint64_t value) { return OSSwapBigToHostInt64(value); }

static int readAt(int fd, void *bytes, size_t size, uint64_t offset, uint64_t fileSize) {
    if (offset > fileSize || size > fileSize - offset) return EINVAL;
    unsigned char *cursor = bytes;
    while (size) {
        ssize_t count = pread(fd, cursor, size, (off_t)offset);
        if (count < 0) { if (errno == EINTR) continue; return errno; }
        if (!count) return EINVAL;
        cursor += count; offset += (size_t)count; size -= (size_t)count;
    }
    return 0;
}

static bool unchanged(const struct stat *before, const struct stat *after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino && before->st_size == after->st_size &&
           before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec && before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
           before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec && before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
}

static size_t directoryHeaderSize(uint32_t version) {
    if (version < 0x20000 || version > 0x20600) return 0;
    if (version >= 0x20600) return sizeof(CodeDirectory);
    if (version >= 0x20500) return offsetof(CodeDirectory, linkageHashType);
    if (version >= 0x20400) return offsetof(CodeDirectory, runtime);
    if (version >= 0x20300) return offsetof(CodeDirectory, executableBase);
    if (version >= 0x20200) return offsetof(CodeDirectory, spare3);
    if (version >= 0x20100) return offsetof(CodeDirectory, teamOffset);
    return offsetof(CodeDirectory, scatterOffset);
}

static bool validString(const unsigned char *bytes, size_t metadataEnd, size_t headerSize, uint32_t offset) {
    return !offset || (offset >= headerSize && offset < metadataEnd && memchr(bytes + offset, 0, metadataEnd - offset));
}

static int validateDirectory(const unsigned char *bytes, size_t size, uint64_t signatureOffset, unsigned *rank) {
    if (size < offsetof(CodeDirectory, scatterOffset)) return EINVAL;
    CodeDirectory directory = {0};
    memcpy(&directory, bytes, size < sizeof(directory) ? size : sizeof(directory));
    if (big32(directory.magic) != CodeDirectoryMagic || big32(directory.length) != size) return EINVAL;
    uint32_t version = big32(directory.version);
    size_t headerSize = directoryHeaderSize(version);
    if (!headerSize) return ENOTSUP;
    if (size < headerSize || directory.spare2 || (version >= 0x20300 && directory.spare3)) return EINVAL;
    // Apple's XNU hash_rank preference, independent of SuperBlob index order.
    // https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/ubc_subr.c
    static const unsigned ranks[] = {0, 1, 3, 2, 4}, hashSizes[] = {0, 20, 32, 20, 48};
    if (directory.hashType < 1 || directory.hashType > 4) return ENOTSUP;
    if (directory.hashSize != hashSizes[directory.hashType] || (directory.pageShift != 12 && directory.pageShift != 14)) return EINVAL;
    *rank = ranks[directory.hashType];
    uint32_t hashOffset = big32(directory.hashOffset), specialSlots = big32(directory.specialSlots), codeSlots = big32(directory.codeSlots);
    if (hashOffset > size || specialSlots > hashOffset / directory.hashSize || codeSlots > (size - hashOffset) / directory.hashSize) return EINVAL;
    size_t metadataEnd = hashOffset - (size_t)specialSlots * directory.hashSize;
    if (metadataEnd < headerSize || !validString(bytes, metadataEnd, headerSize, big32(directory.identifierOffset)) ||
        (version >= 0x20200 && !validString(bytes, metadataEnd, headerSize, big32(directory.teamOffset)))) return EINVAL;
    uint64_t codeLimit = big32(directory.codeLimit);
    if (version >= 0x20300 && directory.codeLimit64) codeLimit = big64(directory.codeLimit64);
    if (codeLimit > signatureOffset) return EINVAL;
    uint32_t scatterOffset = version >= 0x20100 ? big32(directory.scatterOffset) : 0;
    if (scatterOffset) {
        if (scatterOffset < headerSize || scatterOffset > metadataEnd) return EINVAL;
        size_t position = scatterOffset;
        uint64_t pages = 0, previousEnd = 0;
        for (;;) {
            if (sizeof(Scatter) > metadataEnd - position) return EINVAL;
            Scatter scatter; memcpy(&scatter, bytes + position, sizeof(scatter)); position += sizeof(scatter);
            uint32_t count = big32(scatter.count), base = big32(scatter.base);
            if (!count) break;
            uint64_t end = (uint64_t)base + count;
            uint64_t target = big64(scatter.targetOffset), extent = (uint64_t)count << directory.pageShift;
            if (scatter.spare || base < previousEnd || end > UINT32_MAX || pages + count > codeSlots ||
                target > signatureOffset || extent > signatureOffset - target) return EINVAL;
            pages += count; previousEnd = end;
        }
    } else {
        uint64_t pageBytes = UINT64_C(1) << directory.pageShift;
        if (codeSlots != (codeLimit + pageBytes - 1) / pageBytes) return EINVAL;
    }
    if (version >= 0x20400) {
        uint64_t base = big64(directory.executableBase), limit = big64(directory.executableLimit);
        if (base > signatureOffset || limit > signatureOffset - base) return EINVAL;
    }
    if (version >= 0x20500 && directory.preEncryptOffset) {
        uint32_t offset = big32(directory.preEncryptOffset);
        if (offset < headerSize || offset > size || codeSlots > (size - offset) / directory.hashSize) return EINVAL;
    }
    if (version >= 0x20600 && directory.linkageHashType) {
        uint32_t offset = big32(directory.linkageOffset), length = big32(directory.linkageSize);
        if (offset < headerSize || offset > size || length > size - offset) return EINVAL;
    }
    return 0;
}

static int selectDirectory(const unsigned char *bytes, size_t size, uint64_t signatureOffset,
                           const unsigned char **selected, size_t *selectedSize) {
    if (size < sizeof(BlobHeader)) return EINVAL;
    BlobHeader header; memcpy(&header, bytes, sizeof(header));
    size_t length = big32(header.length);
    if (length < sizeof(header) || length > size) return EINVAL;
    if (big32(header.magic) == CodeDirectoryMagic) {
        unsigned rank = 0;
        int error = validateDirectory(bytes, length, signatureOffset, &rank);
        if (error) return error;
        *selected = bytes; *selectedSize = length;
        return 0;
    }
    if (big32(header.magic) != SuperBlobMagic || length < sizeof(SuperBlob)) return EINVAL;
    SuperBlob super; memcpy(&super, bytes, sizeof(super));
    uint32_t count = big32(super.count);
    if (count > MaximumBlobs) return EFBIG;
    if (count > (length - sizeof(super)) / sizeof(BlobIndex)) return EINVAL;
    size_t indexEnd = sizeof(super) + (size_t)count * sizeof(BlobIndex);
    struct { uint32_t slot, offset, length; } ranges[MaximumBlobs];
    unsigned bestRank = 0, seenRanks = 0;
    for (uint32_t i = 0; i < count; ++i) {
        BlobIndex index; memcpy(&index, bytes + sizeof(super) + (size_t)i * sizeof(index), sizeof(index));
        uint32_t slot = big32(index.slot), offset = big32(index.offset);
        if (offset < indexEnd || offset > length || sizeof(BlobHeader) > length - offset) return EINVAL;
        BlobHeader sub; memcpy(&sub, bytes + offset, sizeof(sub));
        uint32_t subLength = big32(sub.length);
        if (subLength < sizeof(sub) || subLength > length - offset) return EINVAL;
        for (uint32_t j = 0; j < i; ++j) {
            if (ranges[j].slot == slot || (offset < ranges[j].offset + ranges[j].length && ranges[j].offset < offset + subLength)) return EINVAL;
        }
        ranges[i].slot = slot; ranges[i].offset = offset; ranges[i].length = subLength;
        if (slot != 0 && !(slot >= AlternateSlot && slot < AlternateLimit)) continue;
        unsigned rank = 0;
        int error = validateDirectory(bytes + offset, subLength, signatureOffset, &rank);
        if (error) return error; // Never fall back to a weaker signature after a malformed alternate.
        if (seenRanks & (1u << rank)) return EINVAL;
        seenRanks |= 1u << rank;
        if (rank > bestRank) { bestRank = rank; *selected = bytes + offset; *selectedSize = subLength; }
    }
    return bestRank ? 0 : ENOENT;
}

int LSBReadMachOCodeDirectoryHash(int fd, unsigned char hash[LSB_GUEST_CDHASH_BYTES]) {
    if (!hash) return EINVAL;
    memset(hash, 0, LSB_GUEST_CDHASH_BYTES);
    struct stat before, after;
    if (fstat(fd, &before)) return errno;
    if (!S_ISREG(before.st_mode) || before.st_size < 0) return EINVAL;
    uint64_t fileSize = (uint64_t)before.st_size;
    struct mach_header_64 header;
    int error = readAt(fd, &header, sizeof(header), 0, fileSize);
    if (error) return error;
    if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64) return ENOEXEC;
    if (header.sizeofcmds > MaximumCommands) return EFBIG;
    if (header.ncmds > header.sizeofcmds / sizeof(struct load_command)) return EINVAL;
    if (!header.sizeofcmds) return ENOENT;
    unsigned char *commands = malloc(header.sizeofcmds);
    if (!commands) return ENOMEM;
    error = readAt(fd, commands, header.sizeofcmds, sizeof(header), fileSize);
    struct linkedit_data_command signature = {0};
    bool found = false;
    size_t position = 0;
    for (uint32_t i = 0; !error && i < header.ncmds; ++i) {
        struct load_command command;
        if (sizeof(command) > header.sizeofcmds - position) { error = EINVAL; break; }
        memcpy(&command, commands + position, sizeof(command));
        if (command.cmdsize < sizeof(command) || command.cmdsize > header.sizeofcmds - position || (command.cmdsize & 7)) {
            error = EINVAL; break;
        }
        if (command.cmd == LC_CODE_SIGNATURE) {
            if (found || command.cmdsize != sizeof(signature)) { error = EINVAL; break; }
            memcpy(&signature, commands + position, sizeof(signature)); found = true;
        }
        position += command.cmdsize;
    }
    if (!error && position != header.sizeofcmds) error = EINVAL;
    free(commands);
    if (error) return error;
    if (!found) return ENOENT;
    if (signature.datasize > MaximumSignature) return EFBIG;
    if (signature.datasize < sizeof(BlobHeader) || signature.dataoff < sizeof(header) + header.sizeofcmds ||
        signature.dataoff > fileSize || signature.datasize > fileSize - signature.dataoff) return EINVAL;
    unsigned char *bytes = malloc(signature.datasize);
    if (!bytes) return ENOMEM;
    error = readAt(fd, bytes, signature.datasize, signature.dataoff, fileSize);
    const unsigned char *selected = NULL;
    size_t selectedSize = 0;
    if (!error) error = selectDirectory(bytes, signature.datasize, signature.dataoff, &selected, &selectedSize);
    unsigned char digest[CC_SHA384_DIGEST_LENGTH] = {0};
    if (!error) {
        CodeDirectory directory = {0}; memcpy(&directory, selected, offsetof(CodeDirectory, scatterOffset));
        switch (directory.hashType) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            case 1: CC_SHA1(selected, (CC_LONG)selectedSize, digest); break;
#pragma clang diagnostic pop
            case 2: case 3: CC_SHA256(selected, (CC_LONG)selectedSize, digest); break;
            case 4: CC_SHA384(selected, (CC_LONG)selectedSize, digest); break;
        }
    }
    free(bytes);
    if (!error && fstat(fd, &after)) error = errno;
    if (!error && !unchanged(&before, &after)) error = EAGAIN;
    if (!error) memcpy(hash, digest, LSB_GUEST_CDHASH_BYTES);
    return error;
}
