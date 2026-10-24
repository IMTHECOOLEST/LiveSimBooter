#include "../LiveSimBooter/CompactImageInfo.cpp"

#include <CoreFoundation/CoreFoundation.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace lsb { pid_t getpid() { return 1234; } }
static unsigned checks;
static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
struct alignas(16384) TestImage { mach_header_64 header; uuid_command uuid; segment_command_64 pageZero, text; };
static TestImage fixtures[3];
struct alignas(16384) CacheImageFixture {
    uint8_t padding[4096];
    mach_header_64 header;
    uuid_command uuid;
    segment_command_64 text, data;
};
static CacheImageFixture cachedFixture;
static_assert(offsetof(CacheImageFixture, header) == 4096, "Exercise a cached image not aligned to the VM page size");
static uint64_t little(const uint8_t *bytes, unsigned size) {
    uint64_t result = 0;
    for (unsigned index = 0; index < size; ++index) result |= uint64_t(bytes[index]) << (index * 8);
    return result;
}
static uint32_t referenceCRC(const uint8_t *bytes, size_t size) {
    uint32_t table[256];
    for (unsigned value = 0; value < 256; ++value) {
        uint32_t entry = value;
        for (unsigned bit = 0; bit < 8; ++bit) entry = (entry & 1) ? ((entry >> 1) ^ 0x82f63b78) : (entry >> 1);
        table[value] = entry;
    }
    uint32_t crc = UINT32_MAX;
    for (size_t index = 0; index < size; ++index) crc = table[(crc ^ bytes[index]) & 255] ^ (crc >> 8);
    return ~crc;
}
static std::vector<uint8_t> currentBytes(const dyld_all_image_infos &info) {
    const auto *start = reinterpret_cast<const uint8_t *>(info.compact_dyld_image_info_addr);
    return {start, start + info.compact_dyld_image_info_size};
}
static CFTypeRef value(CFDictionaryRef dictionary, CFStringRef key) {
    CFTypeRef result = CFDictionaryGetValue(dictionary, key);
    check(result != nullptr, "required property-list key is present");
    return result;
}
static uint64_t number(CFDictionaryRef dictionary, CFStringRef key) {
    auto item = static_cast<CFNumberRef>(value(dictionary, key));
    check(CFGetTypeID(item) == CFNumberGetTypeID(), "integer field has the correct property-list type");
    int64_t result = 0;
    check(CFNumberGetValue(item, kCFNumberSInt64Type, &result), "property-list integer decodes");
    return static_cast<uint64_t>(result);
}
static void checkSnapshot(const dyld_all_image_infos &info, uint64_t state, bool secondHasUUID = true, const char *secondPath = "/RuntimeRoot/bin/df") {
    auto data = currentBytes(info);
    check(data.size() > 78 && std::memcmp(data.data(), "AA01", 4) == 0 && little(data.data() + 4, 2) == 44, "native AA01 archive header");
    check(std::memcmp(data.data() + 6, "TYP1FPATP\x0d\x00process.plistDATB", 28) == 0, "single process.plist regular-file record");
    size_t plistSize = static_cast<size_t>(little(data.data() + 34, 4));
    check(std::memcmp(data.data() + 38, "XATA\x22\x00", 6) == 0 && plistSize + 44 + 34 == data.size(), "archive data and xattr lengths agree");
    const auto *attribute = data.data() + 44 + plistSize;
    check(little(attribute, 4) == 34 && std::memcmp(attribute + 4, "com.apple.dyld.crc32c", 22) == 0, "native dyld CRC attribute");
    check(little(attribute + 26, 4) == data.size(), "CRC attribute records the complete archive length");
    check(referenceCRC(data.data(), data.size() - 4) == little(attribute + 30, 4), "CRC validates archive bytes excluding final checksum");
    check(referenceCRC(data.data(), data.size()) == 0x48674bc7, "complete archive has the native CRC32C residue");
    CFDataRef encoded = CFDataCreate(kCFAllocatorDefault, data.data() + 44, static_cast<CFIndex>(plistSize));
    CFErrorRef error = nullptr;
    CFPropertyListRef decoded = CFPropertyListCreateWithData(kCFAllocatorDefault, encoded, kCFPropertyListImmutable, nullptr, &error);
    check(decoded && !error && CFGetTypeID(decoded) == CFDictionaryGetTypeID(), "independent CoreFoundation binary-plist decoder accepts snapshot");
    auto root = static_cast<CFDictionaryRef>(decoded);
    check(number(root, CFSTR("proc")) == 1234 && number(root, CFSTR("plat")) == PLATFORM_IOSSIMULATOR, "real process ID and simulator platform");
    check(number(root, CFSTR("time")) == info.infoArrayChangeTimestamp && info.infoArrayChangeTimestamp != 0, "blob and legacy timestamps agree and are nonzero");
    check(number(root, CFSTR("stat")) == state && number(root, CFSTR("init")) == info.initialImageCount, "process state and initial count");
    check(number(root, CFSTR("flag")) == 4 && number(root, CFSTR("flags")) == 4, "private-region flags support both atlas spellings");
    auto images = static_cast<CFArrayRef>(value(root, CFSTR("imgs")));
    check(CFGetTypeID(images) == CFArrayGetTypeID() && CFArrayGetCount(images) == 3, "image list includes dyld once");
    const char *paths[] = {"/app/LiveSimBooter", secondPath, "/RuntimeRoot/usr/lib/dyld_sim"};
    for (unsigned index = 0; index < 3; ++index) {
        auto image = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(images, index));
        check(number(image, CFSTR("addr")) == reinterpret_cast<uintptr_t>(&fixtures[index]), "images are sorted by load address");
        auto path = static_cast<CFStringRef>(value(image, CFSTR("file")));
        char pathBuffer[PATH_MAX];
        check(CFStringGetCString(path, pathBuffer, sizeof(pathBuffer), kCFStringEncodingUTF8) && std::strcmp(pathBuffer, paths[index]) == 0,
              "path survives the system binary-plist decoder");
        if (index != 1 || secondHasUUID) {
            auto uuid = static_cast<CFDataRef>(value(image, CFSTR("uuid")));
            check(CFGetTypeID(uuid) == CFDataGetTypeID() && CFDataGetLength(uuid) == 16 &&
                  std::memcmp(CFDataGetBytePtr(uuid), fixtures[index].uuid.uuid, 16) == 0, "UUID bytes round-trip");
        } else check(!CFDictionaryGetValue(image, CFSTR("uuid")), "absent UUID is omitted");
        auto segments = static_cast<CFArrayRef>(value(image, CFSTR("segs")));
        check(CFArrayGetCount(segments) == 1, "PAGEZERO is omitted while mapped segments are retained");
        auto segment = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(segments, 0));
        check(CFEqual(value(segment, CFSTR("name")), CFSTR("__TEXT")), "segment name");
        check(number(image, CFSTR("padr")) == fixtures[index].text.vmaddr && number(segment, CFSTR("padr")) == fixtures[index].text.vmaddr,
              "preferred image and segment addresses");
        check(number(segment, CFSTR("size")) == 32768 && number(segment, CFSTR("fsze")) == 16384 &&
              number(segment, CFSTR("foff")) == 0 && number(segment, CFSTR("perm")) == 5, "segment mapping metadata");
    }
    CFRelease(decoded); CFRelease(encoded);
}
static void expectFailure(dyld_all_image_infos &info, int error, uint64_t state = 0) {
    uintptr_t oldAddress = info.compact_dyld_image_info_addr;
    size_t oldSize = info.compact_dyld_image_info_size;
    uint64_t oldTimestamp = info.infoArrayChangeTimestamp;
    auto oldBytes = currentBytes(info);
    check(lsb::publishCompactImageInfo(&info, state) == error, "invalid input returns expected errno");
    check(info.compact_dyld_image_info_addr == oldAddress && info.compact_dyld_image_info_size == oldSize && info.infoArrayChangeTimestamp == oldTimestamp,
          "failed publication preserves descriptor and timestamp");
    check(currentBytes(info) == oldBytes, "failed publication preserves active bytes");
}
static void checkCachedSnapshot(const dyld_all_image_infos &info) {
    auto bytes = currentBytes(info);
    CFDataRef encoded = CFDataCreate(kCFAllocatorDefault, bytes.data() + 44, static_cast<CFIndex>(little(bytes.data() + 34, 4)));
    CFErrorRef error = nullptr;
    CFPropertyListRef decoded = CFPropertyListCreateWithData(kCFAllocatorDefault, encoded, kCFPropertyListImmutable, nullptr, &error);
    check(decoded && !error && CFGetTypeID(decoded) == CFDictionaryGetTypeID(), "cache snapshot decodes independently");
    auto root = static_cast<CFDictionaryRef>(decoded);
    check(!CFDictionaryGetValue(root, CFSTR("dsc1")), "flattened images do not also appear through a cache bitmap");
    auto images = static_cast<CFArrayRef>(value(root, CFSTR("imgs")));
    check(CFArrayGetCount(images) == 4, "all ordinary and cached images are present exactly once");
    unsigned found = 0;
    for (CFIndex index = 0; index < CFArrayGetCount(images); ++index) {
        auto image = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(images, index));
        if (number(image, CFSTR("addr")) != reinterpret_cast<uintptr_t>(&cachedFixture.header)) continue;
        ++found;
        check(number(image, CFSTR("padr")) == 0x180022000, "cache image preferred address comes from TEXT despite nonzero file offset");
        check(CFEqual(value(image, CFSTR("file")), CFSTR("/usr/lib/libcache-test.dylib")), "cache image install path is retained");
        auto segments = static_cast<CFArrayRef>(value(image, CFSTR("segs")));
        check(CFArrayGetCount(segments) == 2, "cache text and data mappings are both retained");
        auto text = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(segments, 0));
        auto data = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(segments, 1));
        check(number(text, CFSTR("padr")) == 0x180022000 && number(text, CFSTR("foff")) == 0x22000, "packed cache text coordinates");
        check(number(data, CFSTR("padr")) == 0x1800a0128 && number(data, CFSTR("foff")) == 0xa0128 &&
              number(data, CFSTR("size")) == 0x178 && number(data, CFSTR("perm")) == 3, "unaligned cache data coordinates");
        uint64_t slide = number(image, CFSTR("addr")) - number(image, CFSTR("padr"));
        check(number(data, CFSTR("padr")) + slide == reinterpret_cast<uintptr_t>(&cachedFixture.header) + 0x7e128,
              "Atlas address-minus-preferred-address calculation locates the real cached segment");
    }
    check(found == 1, "cached header appears exactly once");
    CFRelease(decoded); CFRelease(encoded);
}
int main(int argc, char **argv) {
    check(crc32c(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xe3069283, "standard CRC32C check value");
    check(crc32c(nullptr, 0) == 0, "empty CRC32C check value");
    for (unsigned index = 0; index < 3; ++index) {
        auto &image = fixtures[index];
        image.header.magic = MH_MAGIC_64; image.header.ncmds = 3;
        image.header.sizeofcmds = sizeof(uuid_command) + 2 * sizeof(segment_command_64);
        image.uuid.cmd = LC_UUID; image.uuid.cmdsize = sizeof(uuid_command);
        for (unsigned byte = 0; byte < 16; ++byte) image.uuid.uuid[byte] = static_cast<uint8_t>(index * 16 + byte);
        image.pageZero.cmd = image.text.cmd = LC_SEGMENT_64;
        image.pageZero.cmdsize = image.text.cmdsize = sizeof(segment_command_64);
        std::strcpy(image.pageZero.segname, "__PAGEZERO"); std::strcpy(image.text.segname, "__TEXT");
        image.text.vmaddr = index == 1 ? 0x100000000 : 0;
        image.text.vmsize = 32768; image.text.filesize = 16384; image.text.initprot = 5;
    }
    const auto *app = reinterpret_cast<const mach_header *>(&fixtures[0]);
    const auto *df = reinterpret_cast<const mach_header *>(&fixtures[1]);
    const auto *dyld = reinterpret_cast<const mach_header *>(&fixtures[2]);
    dyld_image_info images[] = {{dyld, "/RuntimeRoot/usr/lib/dyld_sim", 0}, {app, "/app/LiveSimBooter", 0}, {df, "/RuntimeRoot/bin/df", 0}, {}};
    alignas(16) dyld_all_image_infos info = {};
    info.infoArray = images; info.infoArrayCount = 3; info.initialImageCount = 2; info.platform = PLATFORM_IOSSIMULATOR;
    info.dyldImageLoadAddress = dyld; info.dyldPath = images[0].imageFilePath; info.processDetachedFromSharedRegion = true;
    check(lsb::publishCompactImageInfo(&info) == 0, "initial snapshot publishes");
    checkSnapshot(info, 0x10);
    auto firstSnapshot = currentBytes(info); uintptr_t firstAddress = info.compact_dyld_image_info_addr;
    info.libSystemInitialized = true;
    check(lsb::publishCompactImageInfo(&info) == 0, "second snapshot publishes");
    check(info.compact_dyld_image_info_addr != firstAddress &&
          std::memcmp(reinterpret_cast<const void *>(firstAddress), firstSnapshot.data(), firstSnapshot.size()) == 0, "previous snapshot retained");
    checkSnapshot(info, 0x30);
    fixtures[1].uuid.cmd = LC_SOURCE_VERSION;
    check(lsb::publishCompactImageInfo(&info) == 0, "missing UUID supported"); checkSnapshot(info, 0x30, false);
    fixtures[1].uuid.cmd = LC_UUID;
    images[2].imageFilePath = "/RuntimeRoot/bin/đĩa-😀";
    check(lsb::publishCompactImageInfo(&info) == 0, "UTF-8 paths encode as UTF-16 plist strings");
    checkSnapshot(info, 0x30, true, images[2].imageFilePath);
    images[2].imageFilePath = "/RuntimeRoot/bin/df";
    check(lsb::publishCompactImageInfo(nullptr) == EINVAL, "null metadata rejected");
    info.infoArrayCount = 1025; expectFailure(info, E2BIG); info.infoArrayCount = 3;
    info.infoArray = nullptr; expectFailure(info, EINVAL); info.infoArray = images;
    cachedFixture.header.magic = MH_MAGIC_64; cachedFixture.header.ncmds = 3; cachedFixture.header.flags = MH_DYLIB_IN_CACHE;
    cachedFixture.header.sizeofcmds = sizeof(uuid_command) + 2 * sizeof(segment_command_64);
    cachedFixture.uuid.cmd = LC_UUID; cachedFixture.uuid.cmdsize = sizeof(uuid_command);
    cachedFixture.text.cmd = cachedFixture.data.cmd = LC_SEGMENT_64;
    cachedFixture.text.cmdsize = cachedFixture.data.cmdsize = sizeof(segment_command_64);
    std::strcpy(cachedFixture.text.segname, "__TEXT"); std::strcpy(cachedFixture.data.segname, "__DATA");
    cachedFixture.text.vmaddr = 0x180022000; cachedFixture.text.fileoff = 0x22000;
    cachedFixture.text.vmsize = cachedFixture.text.filesize = 0x2380; cachedFixture.text.initprot = 5;
    cachedFixture.data.vmaddr = 0x1800a0128; cachedFixture.data.fileoff = 0xa0128;
    cachedFixture.data.vmsize = cachedFixture.data.filesize = 0x178; cachedFixture.data.initprot = 3;
    images[3] = {reinterpret_cast<const mach_header *>(&cachedFixture.header), "/usr/lib/libcache-test.dylib", 0};
    info.infoArrayCount = 4; info.sharedCacheBaseAddress = 0x180010000; info.sharedCacheSlide = 0x10000; info.sharedCacheUUID[0] = 42;
    check(lsb::publishCompactImageInfo(&info) == 0, "shared cache and packed Mach-O headers are supported");
    checkCachedSnapshot(info);
    check(info.sharedCacheBaseAddress == 0x180010000 && info.sharedCacheSlide == 0x10000 && info.sharedCacheUUID[0] == 42,
          "snapshot leaves authoritative legacy cache metadata unchanged");
    cachedFixture.header.flags = 0; expectFailure(info, EINVAL); cachedFixture.header.flags = MH_DYLIB_IN_CACHE;
    info.infoArrayCount = 3; info.sharedCacheBaseAddress = info.sharedCacheSlide = 0; info.sharedCacheUUID[0] = 0;
    fixtures[1].header.magic = 0; expectFailure(info, ENOEXEC); fixtures[1].header.magic = MH_MAGIC_64;
    fixtures[1].uuid.cmdsize = 7; expectFailure(info, ENOEXEC); fixtures[1].uuid.cmdsize = sizeof(uuid_command);
    fixtures[1].header.ncmds = 4; expectFailure(info, ENOEXEC); fixtures[1].header.ncmds = 3;
    fixtures[1].header.sizeofcmds = 1024 * 1024 + 1; expectFailure(info, ENOEXEC);
    fixtures[1].header.sizeofcmds = sizeof(uuid_command) + 2 * sizeof(segment_command_64);
    fixtures[1].text.nsects = 1; expectFailure(info, ENOEXEC); fixtures[1].text.nsects = 0;
    images[2].imageLoadAddress = reinterpret_cast<const mach_header *>(reinterpret_cast<uintptr_t>(df) + 4);
    expectFailure(info, EINVAL); images[2].imageLoadAddress = df;
    std::string longPath(PATH_MAX, 'x'); images[2].imageFilePath = longPath.c_str(); expectFailure(info, ENAMETOOLONG);
    images[2].imageFilePath = ""; expectFailure(info, EINVAL);
    images[2].imageFilePath = "\xc0\xaf"; expectFailure(info, EILSEQ); images[2].imageFilePath = "/RuntimeRoot/bin/df";
    sLocked = true; expectFailure(info, EBUSY); sLocked = false;
    check(lsb::publishCompactImageInfo(&info, 0x40) == 0, "before-initializers state advances"); checkSnapshot(info, 0x40);
    fixtures[1].header.magic = 0; expectFailure(info, ENOEXEC, 0x50); fixtures[1].header.magic = MH_MAGIC_64;
    check(lsb::publishCompactImageInfo(&info) == 0, "failed state update does not persist"); checkSnapshot(info, 0x40);
    check(lsb::publishCompactImageInfo(&info, 0x50) == 0, "main-called state advances"); checkSnapshot(info, 0x50);
    check(lsb::publishCompactImageInfo(&info, 0x10) == 0, "initialization state cannot regress"); checkSnapshot(info, 0x50);
    info.errorMessage = "test"; info.terminationFlags = 1;
    check(lsb::publishCompactImageInfo(&info) == 0, "pre-main loader error overrides state"); checkSnapshot(info, 0x20);
    info.terminationFlags = 0;
    check(lsb::publishCompactImageInfo(&info) == 0, "post-main loader error overrides state"); checkSnapshot(info, 0x60);
    info.errorMessage = nullptr;
    check(lsb::publishCompactImageInfo(&info) == 0, "error state does not replace saved progress"); checkSnapshot(info, 0x50);
    expectFailure(info, EINVAL, 0x21);
    if (argc == 2) {
        auto bytes = currentBytes(info); FILE *output = std::fopen(argv[1], "wb");
        check(output && std::fwrite(bytes.data(), 1, bytes.size(), output) == bytes.size(), "export snapshot for independent native-SPI validation");
        std::fclose(output);
    }
    std::printf("Compact image metadata: %u checks passed\n", checks);
}
