#include "GuestEntitlements.h"

#include <errno.h>
#include <mach-o/loader.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { MaximumLoadCommandBytes = 1024 * 1024, MaximumDERDepth = 32, MaximumDERNodes = 65536 };

typedef struct { const unsigned char *bytes; size_t size; } DERSpan;
typedef struct { unsigned tag; DERSpan value; } DERItem;

static int nextItem(DERSpan *input, DERItem *item, unsigned *remainingNodes) {
    if (!*remainingNodes) return EFBIG;
    --*remainingNodes;
    if (input->size < 2) return EINVAL;
    unsigned tag = input->bytes[0], lengthByte = input->bytes[1];
    size_t header = 2, length = lengthByte;
    if ((tag & 31) == 31) return EINVAL; // No multi-octet tags occur in the entitlement grammar.
    if (lengthByte & 128) {
        size_t count = lengthByte & 127;
        if (!count || count > sizeof(size_t) || count > input->size - header || !input->bytes[header]) return EINVAL;
        length = 0;
        for (size_t i = 0; i < count; ++i) length = length << 8 | input->bytes[header + i];
        if (length < 128) return EINVAL; // DER uses the shortest definite length encoding.
        header += count;
    }
    if (length > input->size - header) return EINVAL;
    *item = (DERItem){tag, {input->bytes + header, length}};
    input->bytes += header + length;
    input->size -= header + length;
    return 0;
}

static bool validUTF8(DERSpan value) {
    size_t position = 0;
    while (position < value.size) {
        unsigned first = value.bytes[position++];
        if (first < 128) continue;
        unsigned count, scalar;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; scalar = first & 31; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; scalar = first & 15; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; scalar = first & 7; }
        else return false;
        if (count > value.size - position) return false;
        for (unsigned i = 0; i < count; ++i) {
            unsigned next = value.bytes[position++];
            if ((next & 0xc0) != 0x80) return false;
            scalar = scalar << 6 | (next & 63);
        }
        if ((count == 2 && scalar < 0x800) || (count == 3 && scalar < 0x10000) || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff)) return false;
    }
    return true;
}

static int validateValue(DERItem item, unsigned depth, unsigned *remainingNodes);

static int validateDictionary(DERSpan dictionary, unsigned depth, unsigned *remainingNodes) {
    DERSpan previous = {0};
    bool hasPrevious = false;
    while (dictionary.size) {
        DERItem entry, key, value;
        int error = nextItem(&dictionary, &entry, remainingNodes);
        if (error) return error;
        if (entry.tag != 0x30) return EINVAL;
        if ((error = nextItem(&entry.value, &key, remainingNodes))) return error;
        if (key.tag != 0x0c || !validUTF8(key.value)) return EINVAL;
        if (hasPrevious) {
            size_t common = previous.size < key.value.size ? previous.size : key.value.size;
            int order = memcmp(previous.bytes, key.value.bytes, common);
            if (order > 0 || (!order && previous.size >= key.value.size)) return EINVAL;
        }
        previous = key.value;
        hasPrevious = true;
        if ((error = nextItem(&entry.value, &value, remainingNodes))) return error;
        if (entry.value.size) return EINVAL; // A dictionary entry is exactly one UTF-8 key and one value.
        if ((error = validateValue(value, depth + 1, remainingNodes))) return error;
    }
    return 0;
}

static int validateValue(DERItem item, unsigned depth, unsigned *remainingNodes) {
    if (depth > MaximumDERDepth) return EFBIG;
    switch (item.tag) {
        case 0x01: // DER BOOLEAN: canonical false or true.
            return item.value.size == 1 && (item.value.bytes[0] == 0 || item.value.bytes[0] == 0xff) ? 0 : EINVAL;
        case 0x02: // Signed 64-bit plist integer, without redundant sign-extension octets.
            if (!item.value.size || item.value.size > 8) return EINVAL;
            if (item.value.size > 1 && ((item.value.bytes[0] == 0 && !(item.value.bytes[1] & 128)) ||
                                      (item.value.bytes[0] == 0xff && (item.value.bytes[1] & 128)))) return EINVAL;
            return 0;
        case 0x0c:
            return validUTF8(item.value) ? 0 : EINVAL;
        case 0x30: // Array elements keep their declared order; unlike dictionary keys, duplicates are valid.
            while (item.value.size) {
                DERItem child;
                int error = nextItem(&item.value, &child, remainingNodes);
                if (error) return error;
                if ((error = validateValue(child, depth + 1, remainingNodes))) return error;
            }
            return 0;
        case 0xb0:
            return validateDictionary(item.value, depth, remainingNodes);
        default:
            return EINVAL;
    }
}

int LSBValidateGuestEntitlementsDER(const void *bytes, size_t size) {
    if (!bytes || !size) return EINVAL;
    if (size > LSB_GUEST_ENTITLEMENTS_MAX_DER_SIZE) return EFBIG;
    DERSpan input = {bytes, size};
    DERItem root, version, dictionary;
    unsigned remainingNodes = MaximumDERNodes;
    int error = nextItem(&input, &root, &remainingNodes);
    if (error) return error;
    if (input.size || root.tag != 0x70) return EINVAL;
    if ((error = nextItem(&root.value, &version, &remainingNodes))) return error;
    if (version.tag != 0x02 || version.value.size != 1 || version.value.bytes[0] != 1) return EINVAL;
    if ((error = nextItem(&root.value, &dictionary, &remainingNodes))) return error;
    if (root.value.size || dictionary.tag != 0xb0) return EINVAL;
    return validateValue(dictionary, 0, &remainingNodes);
}

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

static bool named(const char field[16], const char *name) {
    char expected[16] = {0};
    size_t length = strlen(name);
    if (length > sizeof(expected)) return false;
    memcpy(expected, name, length);
    return !memcmp(field, expected, sizeof(expected));
}

static bool fileUnchanged(const struct stat *before, const struct stat *after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino && before->st_size == after->st_size &&
           before->st_mtimespec.tv_sec == after->st_mtimespec.tv_sec && before->st_mtimespec.tv_nsec == after->st_mtimespec.tv_nsec &&
           before->st_ctimespec.tv_sec == after->st_ctimespec.tv_sec && before->st_ctimespec.tv_nsec == after->st_ctimespec.tv_nsec;
}

int LSBReadMachODEREntitlements(int fd, unsigned char **bytes, size_t *size) {
    if (bytes) *bytes = NULL;
    if (size) *size = 0;
    if (!bytes || !size) return EINVAL;
    struct stat before, after;
    if (fstat(fd, &before)) return errno;
    if (!S_ISREG(before.st_mode) || before.st_size < 0) return EINVAL;
    uint64_t fileSize = (uint64_t)before.st_size;
    struct mach_header_64 header;
    int error = readAt(fd, &header, sizeof(header), 0, fileSize);
    if (error) return error;
    if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64) return ENOEXEC;
    if (header.sizeofcmds > MaximumLoadCommandBytes) return EFBIG;
    if (header.ncmds > header.sizeofcmds / sizeof(struct load_command)) return EINVAL;
    if (!header.sizeofcmds) return header.ncmds ? EINVAL : ENOENT;
    unsigned char *commands = malloc(header.sizeofcmds);
    if (!commands) return ENOMEM;
    error = readAt(fd, commands, header.sizeofcmds, sizeof(header), fileSize);
    uint64_t derOffset = 0, derSize = 0;
    size_t position = 0;
    bool found = false;
    for (uint32_t i = 0; !error && i < header.ncmds; ++i) {
        struct load_command command;
        if (sizeof(command) > header.sizeofcmds - position) { error = EINVAL; break; }
        memcpy(&command, commands + position, sizeof(command));
        if (command.cmdsize < sizeof(command) || command.cmdsize > header.sizeofcmds - position || (command.cmdsize & 7)) {
            error = EINVAL; break;
        }
        if (command.cmd == LC_SEGMENT_64) {
            struct segment_command_64 segment;
            if (command.cmdsize < sizeof(segment)) { error = EINVAL; break; }
            memcpy(&segment, commands + position, sizeof(segment));
            if (segment.nsects > (command.cmdsize - sizeof(segment)) / sizeof(struct section_64) ||
                command.cmdsize != sizeof(segment) + (size_t)segment.nsects * sizeof(struct section_64) ||
                segment.fileoff > fileSize || segment.filesize > fileSize - segment.fileoff ||
                segment.vmsize > UINT64_MAX - segment.vmaddr || segment.filesize > segment.vmsize) { error = EINVAL; break; }
            for (uint32_t j = 0; j < segment.nsects; ++j) {
                struct section_64 section;
                memcpy(&section, commands + position + sizeof(segment) + (size_t)j * sizeof(section), sizeof(section));
                if (!named(section.sectname, "__ents_der")) continue;
                if (found || !named(segment.segname, "__TEXT") || !named(section.segname, "__TEXT") ||
                    (section.flags & SECTION_TYPE) != S_REGULAR || section.nreloc || !section.size ||
                    section.addr < segment.vmaddr || section.addr - segment.vmaddr > segment.vmsize ||
                    section.size > segment.vmsize - (section.addr - segment.vmaddr) || section.offset < segment.fileoff ||
                    section.offset - segment.fileoff > segment.filesize || section.size > segment.filesize - (section.offset - segment.fileoff) ||
                    section.addr - segment.vmaddr != section.offset - segment.fileoff || section.offset < sizeof(header) + header.sizeofcmds) {
                    error = EINVAL; break;
                }
                if (section.size > LSB_GUEST_ENTITLEMENTS_MAX_DER_SIZE) { error = EFBIG; break; }
                found = true; derOffset = section.offset; derSize = section.size;
            }
        }
        position += command.cmdsize;
    }
    if (!error && position != header.sizeofcmds) error = EINVAL;
    free(commands);
    if (error) return error;
    if (!found) return ENOENT;
    size_t blobSize = (size_t)derSize + 8;
    unsigned char *blob = malloc(blobSize);
    if (!blob) return ENOMEM;
    error = readAt(fd, blob + 8, (size_t)derSize, derOffset, fileSize);
    if (!error) error = LSBValidateGuestEntitlementsDER(blob + 8, (size_t)derSize);
    if (!error && fstat(fd, &after)) error = errno;
    if (!error && !fileUnchanged(&before, &after)) error = EAGAIN;
    if (error) { free(blob); return error; }
    uint32_t magic = LSB_GUEST_ENTITLEMENTS_DER_MAGIC;
    for (unsigned i = 0; i < 4; ++i) {
        blob[i] = (unsigned char)(magic >> (24 - i * 8));
        blob[i + 4] = (unsigned char)(blobSize >> (24 - i * 8));
    }
    *bytes = blob;
    *size = blobSize;
    return 0;
}
