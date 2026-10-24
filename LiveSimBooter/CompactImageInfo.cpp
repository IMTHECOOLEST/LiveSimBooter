/*
 * Copyright (c) 2021, 2023, 2025 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

// Modified 2026-10-24: emit bounded Apple Archive / process.plist snapshots with executable-local storage and primitives.
// Modified 2026-10-24: describe loaded cache images directly, including their packed headers and relocated segment addresses.
// Format follows Apple's framework/AtlasSchema.swift, common/PropertyList.cpp and the native dyld atlas CRC attribute.
#include "CompactImageInfo.h"
#include "LocalPOSIX.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits.h>
#include <mach-o/loader.h>

namespace {

constexpr size_t kMaxImages = 1024, kMaxSegments = 32;
constexpr size_t kMaxObjects = 64 + kMaxImages * (8 + 8 * kMaxSegments);
constexpr size_t kBufferSize = 256 + kMaxImages * (2 * PATH_MAX + 192 + kMaxSegments * 192);
constexpr size_t kArchiveHeaderSize = 44;
constexpr uintptr_t kPageSize = 16384;
enum Key : uint32_t { Proc, Plat, Time, Stat, Init, Flag, Flags, Imgs, Addr, File, UUID, Segs, Padr, Name, Size, Fsze, Foff, Perm, KeyCount };
const char *const kKeys[] = {"proc", "plat", "time", "stat", "init", "flag", "flags", "imgs", "addr", "file", "uuid", "segs",
                           "padr", "name", "size", "fsze", "foff", "perm"};
struct Image {
    uintptr_t address;
    const char *path;
    size_t pathLength;
    uint8_t uuid[16];
    bool hasUUID;
};
struct alignas(16) Descriptor { uintptr_t address; size_t size; };
static_assert(sizeof(Descriptor) == 16 && offsetof(dyld_all_image_infos, compact_dyld_image_info_addr) % 16 == 0,
              "Compact image descriptor requires a naturally aligned 128-bit store");
static_assert(__atomic_always_lock_free(sizeof(Descriptor), nullptr), "Compact descriptor publication must not call host atomic helpers");
alignas(16) uint8_t sBuffers[2][kBufferSize];
Image sImages[kMaxImages];
uint32_t sObjectOffsets[kMaxObjects], sImageObjects[kMaxImages];
unsigned sActiveBuffer;
uint64_t sInitializationState, sTimestamp;
bool sLocked;

bool readUTF8(const char *text, size_t size, size_t &position, uint32_t &scalar) {
    uint8_t first = static_cast<uint8_t>(text[position++]);
    if (first < 128) { scalar = first; return true; }
    unsigned count;
    uint32_t minimum;
    if (first >= 0xc2 && first <= 0xdf) { count = 1; scalar = first & 31; minimum = 0x80; }
    else if (first >= 0xe0 && first <= 0xef) { count = 2; scalar = first & 15; minimum = 0x800; }
    else if (first >= 0xf0 && first <= 0xf4) { count = 3; scalar = first & 7; minimum = 0x10000; }
    else return false;
    if (count > size - position) return false;
    for (unsigned index = 0; index < count; ++index) {
        uint8_t byte = static_cast<uint8_t>(text[position++]);
        if ((byte & 0xc0) != 0x80) return false;
        scalar = (scalar << 6) | (byte & 63);
    }
    return scalar >= minimum && scalar <= 0x10ffff && (scalar < 0xd800 || scalar > 0xdfff);
}

struct Writer {
    uint8_t *data;
    size_t size;
    uint32_t objectCount;
    bool valid;

    void byte(uint8_t value) {
        if (size == kBufferSize) { valid = false; return; }
        data[size++] = value;
    }
    void little(uint64_t value, unsigned bytes) {
        for (unsigned index = 0; index < bytes; ++index) { byte(static_cast<uint8_t>(value)); value >>= 8; }
    }
    void big(uint64_t value, unsigned bytes) {
        for (unsigned index = bytes; index; --index) byte(static_cast<uint8_t>(value >> ((index - 1) * 8)));
    }
    void bytes(const void *source, size_t count) {
        const auto *input = static_cast<const uint8_t *>(source);
        for (size_t index = 0; index < count; ++index) byte(input[index]);
    }
    uint32_t object() {
        if (objectCount == kMaxObjects) { valid = false; return 0; }
        sObjectOffsets[objectCount] = static_cast<uint32_t>(size - kArchiveHeaderSize);
        return objectCount++;
    }
    void marker(uint8_t type, size_t count) {
        if (count < 15) { byte(type | static_cast<uint8_t>(count)); return; }
        byte(type | 15);
        byte(0x13);
        big(count, 8);
    }
    uint32_t integer(uint64_t value) { uint32_t id = object(); byte(0x13); big(value, 8); return id; }
    uint32_t string(const char *value, size_t length) {
        uint32_t id = object();
        bool ascii = true;
        for (size_t index = 0; index < length; ++index) if (static_cast<uint8_t>(value[index]) >= 128) ascii = false;
        if (ascii) { marker(0x50, length); bytes(value, length); return id; }
        size_t position = 0, units = 0;
        uint32_t scalar;
        while (position < length) {
            if (!readUTF8(value, length, position, scalar)) { valid = false; return id; }
            units += scalar > 0xffff ? 2 : 1;
        }
        marker(0x60, units);
        position = 0;
        while (position < length) {
            if (!readUTF8(value, length, position, scalar)) { valid = false; return id; }
            if (scalar <= 0xffff) big(scalar, 2);
            else { scalar -= 0x10000; big(0xd800 | (scalar >> 10), 2); big(0xdc00 | (scalar & 1023), 2); }
        }
        return id;
    }
    uint32_t dataObject(const uint8_t *value, size_t length) {
        uint32_t id = object(); marker(0x40, length); bytes(value, length); return id;
    }
    uint32_t array(const uint32_t *objects, size_t count) {
        uint32_t id = object(); marker(0xa0, count);
        for (size_t index = 0; index < count; ++index) big(objects[index], 4);
        return id;
    }
    uint32_t dictionary(const uint32_t *keys, const uint32_t *values, size_t count) {
        uint32_t id = object(); marker(0xd0, count);
        for (size_t index = 0; index < count; ++index) big(keys[index], 4);
        for (size_t index = 0; index < count; ++index) big(values[index], 4);
        return id;
    }
};

uint32_t crc32c(const uint8_t *bytes, size_t size) {
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < size; ++index) {
        crc ^= bytes[index];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((0u - (crc & 1)) & 0x82f63b78u);
    }
    return ~crc;
}

void copyImage(Image &destination, const Image &source) {
    destination.address = source.address;
    destination.path = source.path;
    destination.pathLength = source.pathLength;
    destination.hasUUID = source.hasUUID;
    for (unsigned byte = 0; byte < 16; ++byte) destination.uuid[byte] = source.uuid[byte];
}

template <size_t Size> bool hasSegmentName(const segment_command_64 *segment, const char (&name)[Size]) {
    static_assert(Size <= sizeof(segment->segname), "Segment name must fit its fixed-width field");
    for (unsigned index = 0; index < Size; ++index) if (segment->segname[index] != name[index]) return false;
    return true;
}

int addImage(const mach_header *header, const char *path, size_t &count) {
    if (!header || !path || !*path) return EINVAL;
    const uintptr_t address = reinterpret_cast<uintptr_t>(header);
    // Cache builders pack individual Mach-O headers on 4 KiB boundaries inside 16 KiB mappings.
    if (address % 4096 || (address % kPageSize && !(header->flags & MH_DYLIB_IN_CACHE))) return EINVAL;
    for (size_t index = 0; index < count; ++index) if (sImages[index].address == address) return 0;
    if (count == kMaxImages) return E2BIG;
    size_t pathLength = 0;
    while (pathLength < PATH_MAX && path[pathLength]) ++pathLength;
    if (pathLength == PATH_MAX) return ENAMETOOLONG;
    size_t position = 0;
    uint32_t scalar;
    while (position < pathLength) if (!readUTF8(path, pathLength, position, scalar)) return EILSEQ;
    if (header->magic != MH_MAGIC_64 || header->ncmds > 4096 || header->sizeofcmds > 1024 * 1024) return ENOEXEC;
    const size_t headerSize = sizeof(mach_header_64);
    if (address > UINTPTR_MAX - headerSize - header->sizeofcmds) return EOVERFLOW;

    Image &image = sImages[count];
    image.address = address;
    image.path = path;
    image.pathLength = pathLength;
    image.hasUUID = false;
    for (unsigned byte = 0; byte < 16; ++byte) image.uuid[byte] = 0;
    size_t offset = 0, segments = 0;
    const auto *commands = reinterpret_cast<const uint8_t *>(header) + headerSize;
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        if (offset > header->sizeofcmds || header->sizeofcmds - offset < sizeof(load_command)) return ENOEXEC;
        const auto *command = reinterpret_cast<const load_command *>(commands + offset);
        if (command->cmdsize < sizeof(load_command) || command->cmdsize % 8 || command->cmdsize > header->sizeofcmds - offset) return ENOEXEC;
        if (command->cmd == LC_UUID) {
            if (command->cmdsize != sizeof(uuid_command) || image.hasUUID) return ENOEXEC;
            const auto *uuid = reinterpret_cast<const uuid_command *>(command)->uuid;
            for (unsigned byte = 0; byte < 16; ++byte) image.uuid[byte] = uuid[byte];
            image.hasUUID = true;
        } else if (command->cmd == LC_SEGMENT_64) {
            if (command->cmdsize < sizeof(segment_command_64)) return ENOEXEC;
            const auto *segment = reinterpret_cast<const segment_command_64 *>(command);
            if (segment->nsects > (command->cmdsize - sizeof(*segment)) / sizeof(section_64)) return ENOEXEC;
            if (!hasSegmentName(segment, "__PAGEZERO") && ++segments > kMaxSegments) return E2BIG;
        }
        offset += command->cmdsize;
    }
    if (offset != header->sizeofcmds) return ENOEXEC;
    ++count;
    return 0;
}

uint32_t writeImage(Writer &writer, const Image &image) {
    uint32_t segmentObjects[kMaxSegments];
    size_t segmentCount = 0;
    uintptr_t preferredAddress = 0;
    const auto *header = reinterpret_cast<const mach_header_64 *>(image.address);
    const auto *command = reinterpret_cast<const load_command *>(header + 1);
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        if (command->cmd == LC_SEGMENT_64) {
            const auto *segment = reinterpret_cast<const segment_command_64 *>(command);
            if (!hasSegmentName(segment, "__PAGEZERO")) {
                size_t nameLength = 0;
                while (nameLength < sizeof(segment->segname) && segment->segname[nameLength]) ++nameLength;
                uint32_t keys[] = {Name, Padr, Size, Fsze, Foff, Perm};
                uint32_t values[] = {writer.string(segment->segname, nameLength), writer.integer(segment->vmaddr), writer.integer(segment->vmsize),
                                     writer.integer(segment->filesize), writer.integer(segment->fileoff), writer.integer(segment->initprot)};
                segmentObjects[segmentCount++] = writer.dictionary(keys, values, 6);
                // A cache image's __TEXT file offset is relative to its cache file and is usually nonzero.
                if (hasSegmentName(segment, "__TEXT")) preferredAddress = segment->vmaddr;
            }
        }
        command = reinterpret_cast<const load_command *>(reinterpret_cast<const uint8_t *>(command) + command->cmdsize);
    }
    uint32_t keys[] = {Addr, File, Segs, Padr, UUID};
    uint32_t values[5];
    values[0] = writer.integer(image.address);
    values[1] = writer.string(image.path, image.pathLength);
    values[2] = writer.array(segmentObjects, segmentCount);
    values[3] = writer.integer(preferredAddress);
    size_t fields = 4;
    if (image.hasUUID) { values[4] = writer.dataObject(image.uuid, 16); fields = 5; }
    return writer.dictionary(keys, values, fields);
}

int buildSnapshot(dyld_all_image_infos *info, Writer &writer, uint64_t timestamp, uint64_t state) {
    if ((info->infoArrayCount && !info->infoArray)) return EINVAL;
    if (info->infoArrayCount > kMaxImages) return E2BIG;
    // Atlas permits complete image records in imgs without dsc1. Flatten loaded cache images here so LLDB can consume their real segments.
    // Cache-specific APIs and cache bitmap introspection are not represented; the legacy sharedCache fields remain owned by dyld_sim.
    size_t count = 0;
    for (uint32_t index = 0; index < info->infoArrayCount; ++index) {
        const dyld_image_info &image = info->infoArray[index];
        int error = addImage(image.imageLoadAddress, image.imageFilePath, count);
        if (error) return error;
    }
    int error = addImage(info->dyldImageLoadAddress, info->dyldPath, count);
    if (error) return error;
    for (size_t index = 1; index < count; ++index) {
        Image value;
        copyImage(value, sImages[index]);
        size_t position = index;
        while (position && sImages[position - 1].address > value.address) {
            copyImage(sImages[position], sImages[position - 1]);
            --position;
        }
        copyImage(sImages[position], value);
    }
    // One uncompressed AA01 regular-file record. DATB is a 32-bit data length; XATA holds a 16-bit xattr length.
    writer.bytes("AA01", 4); writer.little(kArchiveHeaderSize, 2);
    writer.bytes("TYP1FPATP", 9); writer.little(13, 2); writer.bytes("process.plist", 13);
    writer.bytes("DATB", 4); writer.little(0, 4); writer.bytes("XATA", 4); writer.little(34, 2);
    writer.bytes("bplist00", 8);
    for (uint32_t index = 0; index < KeyCount; ++index) {
        size_t length = 0;
        while (kKeys[index][length]) ++length;
        writer.string(kKeys[index], length);
    }
    uint32_t keys[] = {Proc, Plat, Time, Stat, Init, Flag, Flags, Imgs};
    uint32_t values[8];
    values[0] = writer.integer(static_cast<uint64_t>(lsb::getpid()));
    values[1] = writer.integer(info->platform);
    values[2] = writer.integer(timestamp);
    if (info->errorMessage) state = info->terminationFlags ? 0x20 : 0x60;
    values[3] = writer.integer(state);
    values[4] = writer.integer(info->initialImageCount);
    values[5] = values[6] = writer.integer(info->processDetachedFromSharedRegion ? 4 : 0);
    for (size_t index = 0; index < count; ++index) sImageObjects[index] = writeImage(writer, sImages[index]);
    values[7] = writer.array(sImageObjects, count);
    uint32_t root = writer.dictionary(keys, values, 8);
    size_t offsetTable = writer.size - kArchiveHeaderSize;
    for (uint32_t index = 0; index < writer.objectCount; ++index) writer.big(sObjectOffsets[index], 4);
    writer.big(0, 6); writer.byte(4); writer.byte(4);
    writer.big(writer.objectCount, 8); writer.big(root, 8); writer.big(offsetTable, 8);
    size_t plistSize = writer.size - kArchiveHeaderSize;
    // The native dyld CRC xattr describes the whole archive; its final four-byte checksum is excluded from the CRC input.
    writer.little(34, 4); writer.bytes("com.apple.dyld.crc32c", 22); writer.little(writer.size + 8, 4);
    if (!writer.valid || writer.size > kBufferSize - 4) return E2BIG;
    for (unsigned byte = 0; byte < 4; ++byte) writer.data[34 + byte] = static_cast<uint8_t>(plistSize >> (byte * 8));
    writer.little(crc32c(writer.data, writer.size), 4);
    return 0;
}

} // namespace

namespace lsb {

int publishCompactImageInfo(dyld_all_image_infos *info, uint64_t state) {
    if (!info || reinterpret_cast<uintptr_t>(&info->compact_dyld_image_info_addr) % alignof(Descriptor)) return EINVAL;
    if (state && state != 0x10 && state != 0x30 && state != 0x40 && state != 0x50) return EINVAL;
    if (__atomic_test_and_set(&sLocked, __ATOMIC_ACQUIRE)) return EBUSY;
    unsigned buffer = sActiveBuffer ^ 1;
    uint64_t timestamp = info->infoArrayChangeTimestamp;
    if (timestamp <= sTimestamp) timestamp = sTimestamp + 1;
    if (!timestamp) { __atomic_clear(&sLocked, __ATOMIC_RELEASE); return EOVERFLOW; }
    uint64_t nextState = info->libSystemInitialized ? 0x30 : 0x10;
    if (sInitializationState > nextState) nextState = sInitializationState;
    if (state > nextState) nextState = state;
    Writer writer = {sBuffers[buffer], 0, 0, true};
    int error = buildSnapshot(info, writer, timestamp, nextState);
    if (!error) {
        Descriptor descriptor = {reinterpret_cast<uintptr_t>(writer.data), writer.size};
        auto *destination = reinterpret_cast<Descriptor *>(&info->compact_dyld_image_info_addr);
        __atomic_store(destination, &descriptor, __ATOMIC_RELEASE);
        info->infoArrayChangeTimestamp = timestamp;
        sActiveBuffer = buffer;
        sTimestamp = timestamp;
        sInitializationState = nextState;
    }
    __atomic_clear(&sLocked, __ATOMIC_RELEASE);
    return error;
}

} // namespace lsb
