#include "../ProcessSupport/GuestCodeSignature.h"

#include <CommonCrypto/CommonDigest.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { unsigned char *bytes; size_t size; } Bytes;

static void put32(void *destination, uint32_t value) {
    unsigned char *bytes = destination;
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (24 - i * 8));
}

static Bytes directory(unsigned type, uint32_t version) {
    static const unsigned hashSizes[] = {0, 20, 32, 20, 48};
    Bytes result = {calloc(1, 128 + 3 * hashSizes[type]), 128 + 3 * hashSizes[type]};
    assert(result.bytes);
    put32(result.bytes, 0xfade0c02); put32(result.bytes + 4, (uint32_t)result.size);
    put32(result.bytes + 8, version); put32(result.bytes + 16, 128 + 2 * hashSizes[type]);
    put32(result.bytes + 20, 112); put32(result.bytes + 24, 2); put32(result.bytes + 28, 1); put32(result.bytes + 32, 4096);
    result.bytes[36] = hashSizes[type]; result.bytes[37] = type; result.bytes[39] = 12;
    memcpy(result.bytes + 112, "test.image", 11);
    for (size_t i = 128; i < result.size; ++i) result.bytes[i] = (unsigned char)i;
    return result;
}

static Bytes superBlob(const Bytes *directories, const uint32_t *slots, size_t count) {
    size_t size = 12 + count * 8;
    for (size_t i = 0; i < count; ++i) size += directories[i].size;
    Bytes result = {calloc(1, size), size}; assert(result.bytes);
    put32(result.bytes, 0xfade0cc0); put32(result.bytes + 4, (uint32_t)size); put32(result.bytes + 8, (uint32_t)count);
    size_t offset = 12 + count * 8;
    for (size_t i = 0; i < count; ++i) {
        put32(result.bytes + 12 + i * 8, slots[i]); put32(result.bytes + 16 + i * 8, (uint32_t)offset);
        memcpy(result.bytes + offset, directories[i].bytes, directories[i].size); offset += directories[i].size;
    }
    return result;
}

static Bytes image(const Bytes *signature) {
    Bytes result = {calloc(1, 4096 + signature->size + 16), 4096 + signature->size + 16}; assert(result.bytes);
    struct mach_header_64 header = {.magic = MH_MAGIC_64, .cputype = CPU_TYPE_ARM64, .cpusubtype = CPU_SUBTYPE_ARM64_ALL,
        .filetype = MH_EXECUTE, .ncmds = 1, .sizeofcmds = sizeof(struct linkedit_data_command)};
    struct linkedit_data_command command = {.cmd = LC_CODE_SIGNATURE, .cmdsize = sizeof(command),
        .dataoff = 4096, .datasize = (uint32_t)signature->size + 16};
    memcpy(result.bytes, &header, sizeof(header)); memcpy(result.bytes + sizeof(header), &command, sizeof(command));
    memcpy(result.bytes + command.dataoff, signature->bytes, signature->size);
    memset(result.bytes + command.dataoff + signature->size, 0xa5, 16); // Unused LC_CODE_SIGNATURE padding is not hashed.
    return result;
}

static int fixtureFD(const Bytes *bytes) {
    char path[] = "/tmp/lsb-code-signature-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); assert(!unlink(path));
    assert(write(fd, bytes->bytes, bytes->size) == (ssize_t)bytes->size);
    assert(lseek(fd, 17, SEEK_SET) == 17);
    return fd;
}

static void checkImage(const Bytes *bytes, int expected, const unsigned char *expectedHash) {
    int fd = fixtureFD(bytes);
    unsigned char hash[LSB_GUEST_CDHASH_BYTES]; memset(hash, 0xa5, sizeof(hash));
    int error = LSBReadMachOCodeDirectoryHash(fd, hash);
    if (error != expected) fprintf(stderr, "Expected reader error %d, received %d\n", expected, error);
    assert(error == expected);
    const unsigned char zero[LSB_GUEST_CDHASH_BYTES] = {0};
    assert(!memcmp(hash, expected ? zero : expectedHash, sizeof(hash)));
    assert(lseek(fd, 0, SEEK_CUR) == 17); assert(!close(fd));
}

static void checkSignature(const Bytes *signature, int expected, const unsigned char *expectedHash) {
    Bytes executable = image(signature); checkImage(&executable, expected, expectedHash); free(executable.bytes);
}

static void digest(const Bytes *bytes, unsigned type, unsigned char hash[LSB_GUEST_CDHASH_BYTES]) {
    unsigned char complete[CC_SHA384_DIGEST_LENGTH] = {0};
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    if (type == 1) CC_SHA1(bytes->bytes, (CC_LONG)bytes->size, complete);
#pragma clang diagnostic pop
    else if (type == 4) CC_SHA384(bytes->bytes, (CC_LONG)bytes->size, complete);
    else CC_SHA256(bytes->bytes, (CC_LONG)bytes->size, complete);
    memcpy(hash, complete, LSB_GUEST_CDHASH_BYTES);
}

static void testSelection(void) {
    unsigned char expected[LSB_GUEST_CDHASH_BYTES], previous[LSB_GUEST_CDHASH_BYTES] = {0};
    for (unsigned type = 1; type <= 4; ++type) {
        for (uint32_t version = 0x20000; version <= 0x20600; version += 0x100) {
            Bytes cd = directory(type, version); digest(&cd, type, expected); checkSignature(&cd, 0, expected);
            uint32_t slot = 0; Bytes signature = superBlob(&cd, &slot, 1); checkSignature(&signature, 0, expected);
            free(signature.bytes); free(cd.bytes);
        }
    }
    const unsigned types[] = {1, 3, 2, 4};
    uint32_t slots[] = {0, 0x1000, 0x1001, 0x1002};
    Bytes cds[4];
    for (unsigned i = 0; i < 4; ++i) cds[i] = directory(types[i], 0x20400);
    for (unsigned count = 1; count <= 4; ++count) {
        digest(&cds[count - 1], types[count - 1], expected);
        assert(memcmp(previous, expected, sizeof(expected))); memcpy(previous, expected, sizeof(expected));
        Bytes signature = superBlob(cds, slots, count); checkSignature(&signature, 0, expected); free(signature.bytes);
    }
    Bytes reversed[] = {cds[3], cds[2], cds[1], cds[0]};
    Bytes signature = superBlob(reversed, slots, 4); checkSignature(&signature, 0, expected); free(signature.bytes);
    // Different signed images, despite the same identifier and algorithm, must not share the cache key.
    digest(&cds[2], 2, previous); cds[2].bytes[cds[2].size - 1] ^= 0x40; digest(&cds[2], 2, expected);
    assert(memcmp(previous, expected, sizeof(expected))); checkSignature(&cds[2], 0, expected);
    for (unsigned i = 0; i < 4; ++i) free(cds[i].bytes);
}

static void mutate32(const Bytes *original, size_t offset, uint32_t value, int expected) {
    Bytes modified = {malloc(original->size), original->size}; assert(modified.bytes);
    memcpy(modified.bytes, original->bytes, original->size); put32(modified.bytes + offset, value);
    checkSignature(&modified, expected, NULL); free(modified.bytes);
}

static void testMalformedSignatures(void) {
    Bytes cd = directory(2, 0x20600);
    const struct { size_t offset; uint32_t value; int error; } invalid[] = {
        {0, 0xfade0b01, EINVAL}, {4, 7, EINVAL}, {4, UINT32_MAX, EINVAL}, {8, 0x20700, ENOTSUP}, {8, 0x10000, ENOTSUP},
        {16, UINT32_MAX, EINVAL}, {16, 0, EINVAL}, {20, 4, EINVAL}, {20, 129, EINVAL}, {24, UINT32_MAX, EINVAL},
        {28, UINT32_MAX, EINVAL}, {28, 0, EINVAL}, {32, 4097, EINVAL}, {40, 1, EINVAL}, {44, 127, EINVAL},
        {44, 40, EINVAL}, {48, 128, EINVAL}, {52, 1, EINVAL}, {64, 1, EINVAL}, {72, 1, EINVAL}, {92, UINT32_MAX, EINVAL}
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) mutate32(&cd, invalid[i].offset, invalid[i].value, invalid[i].error);
    for (size_t offset = 36; offset <= 39; ++offset) {
        if (offset == 38) continue; // Platform is not part of hash selection.
        unsigned char previous = cd.bytes[offset]; cd.bytes[offset] = 255;
        checkSignature(&cd, offset == 37 ? ENOTSUP : EINVAL, NULL); cd.bytes[offset] = previous;
    }
    unsigned char saved[16]; memcpy(saved, cd.bytes + 112, sizeof(saved));
    memset(cd.bytes + 112, 'x', 16); checkSignature(&cd, EINVAL, NULL); memcpy(cd.bytes + 112, saved, sizeof(saved));
    cd.bytes[96] = 2; put32(cd.bytes + 100, UINT32_MAX); checkSignature(&cd, EINVAL, NULL);
    cd.bytes[96] = 0; put32(cd.bytes + 100, 0);
    Bytes shortHeader = {cd.bytes, 100}; put32(cd.bytes + 4, 100); checkSignature(&shortHeader, EINVAL, NULL);
    put32(cd.bytes + 4, (uint32_t)cd.size);
    Bytes empty = {NULL, 0}; uint32_t noSlot = 0;
    Bytes signature = superBlob(&empty, &noSlot, 0); checkSignature(&signature, ENOENT, NULL); free(signature.bytes);

    Bytes cds[] = {directory(1, 0x20400), cd}; uint32_t slots[] = {0, 0x1000};
    signature = superBlob(cds, slots, 2);
    mutate32(&signature, 8, UINT32_MAX, EFBIG); mutate32(&signature, 8, 1000, EINVAL);
    mutate32(&signature, 16, 12, EINVAL); mutate32(&signature, 16, (uint32_t)signature.size, EINVAL);
    mutate32(&signature, 24, 28, EINVAL); // Aliasing subblobs.
    mutate32(&signature, 20, 0, EINVAL); // Duplicate primary slot.
    mutate32(&signature, 28 + 4, UINT32_MAX, EINVAL);
    mutate32(&signature, 28 + cds[0].size, 0xfade0b01, EINVAL); // Bad stronger candidate cannot fall back.
    free(signature.bytes);
    Bytes duplicate[] = {cd, cd}; signature = superBlob(duplicate, slots, 2); checkSignature(&signature, EINVAL, NULL); free(signature.bytes);
    free(cds[0].bytes); free(cd.bytes);
}

static void testOptionalFields(void) {
    Bytes cd = directory(2, 0x20600);
    unsigned char expected[LSB_GUEST_CDHASH_BYTES];
    put32(cd.bytes + 48, 123); memcpy(cd.bytes + 123, "TEAM", 5);
    digest(&cd, 2, expected); checkSignature(&cd, 0, expected);
    put32(cd.bytes + 48, 0); memset(cd.bytes + 123, 0, 5);
    put32(cd.bytes + 32, 0); put32(cd.bytes + 60, 4096); // Versioned 64-bit code limit.
    digest(&cd, 2, expected); checkSignature(&cd, 0, expected);
    put32(cd.bytes + 32, UINT32_MAX); // A nonzero 64-bit limit takes precedence over the legacy field.
    digest(&cd, 2, expected); checkSignature(&cd, 0, expected);
    put32(cd.bytes + 92, 192); // Reusing a bounded hash array does not affect hashing the directory.
    digest(&cd, 2, expected); checkSignature(&cd, 0, expected);
    cd.bytes[96] = 2; put32(cd.bytes + 100, 128); put32(cd.bytes + 104, 32);
    digest(&cd, 2, expected); checkSignature(&cd, 0, expected);
    put32(cd.bytes + 104, UINT32_MAX); checkSignature(&cd, EINVAL, NULL);
    free(cd.bytes);
    // Scatter entries must include a terminator entirely before the hash arrays.
    cd = directory(2, 0x20100); put32(cd.bytes + 20, 0); put32(cd.bytes + 44, 48);
    put32(cd.bytes + 48, 1); put32(cd.bytes + 52, 0);
    digest(&cd, 2, expected); checkSignature(&cd, 0, expected);
    put32(cd.bytes + 72, 2); checkSignature(&cd, EINVAL, NULL); put32(cd.bytes + 72, 0);
    put32(cd.bytes + 44, 120); checkSignature(&cd, EINVAL, NULL); free(cd.bytes);
}

static void testMachO(void) {
    Bytes cd = directory(2, 0x20400), executable = image(&cd);
    unsigned char hash[LSB_GUEST_CDHASH_BYTES];
    assert(LSBReadMachOCodeDirectoryHash(-1, hash) == EBADF);
    assert(LSBReadMachOCodeDirectoryHash(-1, NULL) == EINVAL);
    int pipeFDs[2]; assert(!pipe(pipeFDs)); assert(LSBReadMachOCodeDirectoryHash(pipeFDs[0], hash) == EINVAL);
    close(pipeFDs[0]); close(pipeFDs[1]);
    struct mach_header_64 *header = (void *)executable.bytes;
    struct linkedit_data_command *command = (void *)(executable.bytes + sizeof(*header));
    header->cputype = CPU_TYPE_X86_64; checkImage(&executable, ENOEXEC, NULL); header->cputype = CPU_TYPE_ARM64;
    header->magic = MH_MAGIC; checkImage(&executable, ENOEXEC, NULL); header->magic = MH_MAGIC_64;
    header->sizeofcmds = 1024 * 1024 + 1; checkImage(&executable, EFBIG, NULL); header->sizeofcmds = sizeof(*command);
    header->ncmds = UINT32_MAX; checkImage(&executable, EINVAL, NULL); header->ncmds = 1;
    command->cmdsize = 15; checkImage(&executable, EINVAL, NULL); command->cmdsize = sizeof(*command);
    command->dataoff = 8; checkImage(&executable, EINVAL, NULL); command->dataoff = 4096;
    command->datasize = 16 * 1024 * 1024 + 1; checkImage(&executable, EFBIG, NULL); command->datasize = (uint32_t)cd.size + 16;
    header->ncmds = 2; header->sizeofcmds = 2 * sizeof(*command);
    memcpy(executable.bytes + sizeof(*header) + sizeof(*command), command, sizeof(*command));
    checkImage(&executable, EINVAL, NULL); header->ncmds = 1; header->sizeofcmds = sizeof(*command);
    command->cmd = LC_FUNCTION_STARTS; checkImage(&executable, ENOENT, NULL); command->cmd = LC_CODE_SIGNATURE;
    size_t size = executable.size;
    for (size_t n = 0; n < sizeof(*header) + sizeof(*command); ++n) { executable.size = n; checkImage(&executable, EINVAL, NULL); }
    for (size_t n = 4096; n < size; ++n) { executable.size = n; checkImage(&executable, EINVAL, NULL); }
    free(executable.bytes); free(cd.bytes);
}

static void actualImage(const char *path, const char *expectedHex) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); assert(fd >= 0);
    unsigned char hash[LSB_GUEST_CDHASH_BYTES];
    int error = LSBReadMachOCodeDirectoryHash(fd, hash);
    if (error) fprintf(stderr, "%s: reader error %d (%s)\n", path, error, strerror(error));
    assert(!error); close(fd);
    char hex[LSB_GUEST_CDHASH_BYTES * 2 + 1];
    for (unsigned i = 0; i < sizeof(hash); ++i) snprintf(hex + i * 2, 3, "%02x", hash[i]);
    if (expectedHex) assert(!strcmp(hex, expectedHex));
    printf("CDHash %s %s\n", hex, path);
}

int main(int argc, char **argv) {
    testSelection(); testMalformedSignatures(); testOptionalFields(); testMachO();
    for (int i = 1; i < argc; i += 2) actualImage(argv[i], i + 1 < argc ? argv[i + 1] : NULL);
    puts("Guest CodeDirectory hash tests passed");
    return 0;
}
