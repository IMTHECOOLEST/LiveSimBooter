// Host tests use synthetic Mach-O bytes and a fake prepare entry. No simulator code is signed or executed.
#include "../LiveSimBooter/SimSyscallHelpers.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>
#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <string>
#include <sys/wait.h>
#include <vector>

static bool rejectSegmentMapping = false;
static int sourceFD = -1, signatureCalls = 0, deallocations = 0, fileUnmaps = 0;
static void *sourceMapping = MAP_FAILED;
static off_t registeredSlice = -1;
static uintptr_t registeredSignature = 0;
static unsigned randomCalls = 0;
static constexpr uintptr_t freshPtrMungeToken = 0xa739c450e16b28df;

static void testArc4RandomBuf(void *buffer, size_t size) {
    if (size != sizeof(uintptr_t)) { fprintf(stderr, "Unexpected pointer token size\n"); exit(1); }
    uintptr_t value = randomCalls++ ? freshPtrMungeToken : 0;
    memcpy(buffer, &value, size);
}

static int testFcntl(int, int command, ...) {
    if (command != F_ADDFILESIGS_RETURN) { errno = EINVAL; return -1; }
    va_list arguments;
    va_start(arguments, command);
    auto *signature = va_arg(arguments, fsignatures_t *);
    va_end(arguments);
    ++signatureCalls;
    registeredSlice = signature->fs_file_start;
    registeredSignature = reinterpret_cast<uintptr_t>(signature->fs_blob_start);
    signature->fs_file_start = static_cast<off_t>(registeredSignature);
    return 0;
}

static void *testMmap(void *address, size_t size, int protection, int flags, int fd, off_t offset) {
    if ((flags & MAP_FIXED) && rejectSegmentMapping) { errno = EACCES; return MAP_FAILED; }
    void *result = ::mmap(address, size, protection, flags, fd, offset);
    if (!(flags & (MAP_FIXED | MAP_ANON))) { sourceFD = fd; sourceMapping = result; }
    return result;
}

static int testMunmap(void *address, size_t size) {
    if (address == sourceMapping) ++fileUnmaps;
    return ::munmap(address, size);
}

static kern_return_t testDeallocate(vm_map_t task, vm_address_t address, vm_size_t size) {
    ++deallocations;
    return ::vm_deallocate(task, address, size);
}

#define fcntl testFcntl
#define mmap testMmap
#define munmap testMunmap
#define vm_deallocate testDeallocate
#define arc4random_buf testArc4RandomBuf
#include "../LiveSimBooter/DyldSimLoader.cpp"
#undef arc4random_buf
#undef vm_deallocate
#undef munmap
#undef mmap
#undef fcntl

static unsigned checks = 0;
static void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}

static std::string expectedMainStack() {
    char value[96];
    snprintf(value, sizeof(value), "main_stack=0x%llx,0x%llx,0x0,0x0",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(pthread_self()))),
             static_cast<unsigned long long>(pthread_get_stacksize_np(pthread_self())));
    return value;
}

static void checkAndConsumeMainStack(const char *value) {
    check(std::string(value) == expectedMainStack(), "main_stack uses real native bounds and zero allocation metadata");
    memset(const_cast<char *>(value) + 11, 0, strlen(value) - 11);
    check(std::string(value) == "main_stack=", "simulator can consume and erase its own main stack metadata");
}

static void mainStackBoundsTest() {
    check(mainStackContains(0x18000, 0x8000, 0x10000), "native stack includes its bottom address");
    check(mainStackContains(0x18000, 0x8000, 0x17fff), "native stack includes its final byte");
    check(!mainStackContains(0x18000, 0x8000, 0x18000), "native stack excludes its top address");
    check(!mainStackContains(0x18000, 0x8000, 0xffff), "native stack rejects an address below its allocation");
    check(!mainStackContains(0x18000, 0, 0x17000), "native stack size must be nonzero");
    check(!mainStackContains(0, 0x8000, 0), "native stack top must be nonzero");
    check(!mainStackContains(0x18000, SIZE_MAX, 0x17000), "native stack subtraction cannot wrap");
    check(!mainStackContains(0x18000, 0x18000, 0x17000), "native stack cannot extend to the null page");
}

using Bytes = std::vector<uint8_t>;
static constexpr uint64_t preferred = 0x100000000, entryOffset = 1024;
static const size_t page = static_cast<size_t>(getpagesize());

template <typename T> static void put(Bytes &bytes, size_t offset, const T &value) {
    check(offset <= bytes.size() && sizeof(value) <= bytes.size() - offset, "fixture write bounds");
    memcpy(bytes.data() + offset, &value, sizeof(value));
}

template <typename T> static void edit(Bytes &bytes, size_t offset, const std::function<void(T &)> &change) {
    T value = read<T>(bytes.data() + offset);
    change(value);
    put(bytes, offset, value);
}

static constexpr size_t textCommand = sizeof(mach_header_64);
static constexpr size_t dataCommand = textCommand + sizeof(segment_command_64);
static constexpr size_t linkeditCommand = dataCommand + sizeof(segment_command_64);
static constexpr size_t signatureCommand = linkeditCommand + sizeof(segment_command_64);
static constexpr size_t entryCommand = signatureCommand + sizeof(linkedit_data_command);

static Bytes thin(bool mainEntry = false) {
    Bytes bytes(3 * page);
    segment_command_64 segment = {};
    segment.cmd = LC_SEGMENT_64;
    segment.cmdsize = sizeof(segment);
    strcpy(segment.segname, SEG_TEXT);
    segment.vmaddr = preferred;
    segment.vmsize = segment.filesize = page;
    segment.initprot = segment.maxprot = VM_PROT_READ | VM_PROT_EXECUTE;
    put(bytes, textCommand, segment);
    strcpy(segment.segname, SEG_DATA);
    segment.vmaddr += page;
    segment.fileoff = page;
    segment.vmsize = 2 * page;
    segment.initprot = segment.maxprot = VM_PROT_READ | VM_PROT_WRITE;
    put(bytes, dataCommand, segment);
    strcpy(segment.segname, SEG_LINKEDIT);
    segment.vmaddr = preferred + 3 * page;
    segment.fileoff = 2 * page;
    segment.vmsize = page;
    segment.initprot = segment.maxprot = VM_PROT_READ;
    put(bytes, linkeditCommand, segment);
    linkedit_data_command signature = {LC_CODE_SIGNATURE, sizeof(signature), static_cast<uint32_t>(bytes.size() - 64), 64};
    put(bytes, signatureCommand, signature);
    uint32_t entrySize;
    if (mainEntry) {
        entry_point_command command = {LC_MAIN, sizeof(command), entryOffset, 0};
        put(bytes, entryCommand, command);
        entrySize = sizeof(command);
    } else {
        entrySize = sizeof(load_command) + 8 + 68 * sizeof(uint32_t);
        load_command command = {LC_UNIXTHREAD, entrySize};
        put(bytes, entryCommand, command);
        put<uint32_t>(bytes, entryCommand + 8, 6);
        put<uint32_t>(bytes, entryCommand + 12, 68);
        put<uint64_t>(bytes, entryCommand + 16 + 32 * sizeof(uint64_t), preferred + entryOffset);
    }
    mach_header_64 header = {MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, MH_DYLINKER, 5,
                             static_cast<uint32_t>(entryCommand + entrySize - sizeof(header)), 0, 0};
    put(bytes, 0, header);
    return bytes;
}

static constexpr size_t buildCommand = entryCommand + sizeof(entry_point_command);
static constexpr size_t pagezeroCommand = buildCommand + sizeof(build_version_command);

static Bytes executable() {
    Bytes bytes = thin(true);
    build_version_command build = {LC_BUILD_VERSION, sizeof(build), PLATFORM_IOSSIMULATOR, 0x110000, 0x110000, 0};
    put(bytes, buildCommand, build);
    segment_command_64 pagezero = {};
    pagezero.cmd = LC_SEGMENT_64;
    pagezero.cmdsize = sizeof(pagezero);
    strcpy(pagezero.segname, SEG_PAGEZERO);
    pagezero.vmsize = preferred;
    put(bytes, pagezeroCommand, pagezero);
    edit<mach_header_64>(bytes, 0, [](auto &header) {
        header.filetype = MH_EXECUTE;
        header.flags = MH_PIE;
        header.ncmds += 2;
        header.sizeofcmds += sizeof(build_version_command) + sizeof(segment_command_64);
    });
    return bytes;
}

static Bytes dylibVariant(Bytes bytes) {
    edit<mach_header_64>(bytes, 0, [](auto &header) { header.filetype = MH_DYLIB; });
    return bytes;
}

static Bytes universal(bool is64, bool swapped = true, const Bytes &slice = thin()) {
    Bytes bytes(page + slice.size());
    auto s32 = [swapped](uint32_t value) { return swapped ? OSSwapInt32(value) : value; };
    auto s64 = [swapped](uint64_t value) { return swapped ? OSSwapInt64(value) : value; };
    fat_header header = {is64 ? FAT_MAGIC_64 : FAT_MAGIC, 1};
    header.magic = s32(header.magic);
    header.nfat_arch = s32(header.nfat_arch);
    put(bytes, 0, header);
    uint32_t alignment = 0;
    while ((size_t(1) << alignment) < page) ++alignment;
    if (is64) {
        fat_arch_64 arch = {};
        arch.cputype = s32(CPU_TYPE_ARM64); arch.cpusubtype = s32(CPU_SUBTYPE_ARM64_ALL);
        arch.offset = s64(page); arch.size = s64(slice.size()); arch.align = s32(alignment);
        put(bytes, sizeof(header), arch);
    } else {
        fat_arch arch = {};
        arch.cputype = s32(CPU_TYPE_ARM64); arch.cpusubtype = s32(CPU_SUBTYPE_ARM64_ALL);
        arch.offset = s32(page); arch.size = s32(slice.size()); arch.align = s32(alignment);
        put(bytes, sizeof(header), arch);
    }
    memcpy(bytes.data() + page, slice.data(), slice.size());
    return bytes;
}

static void parse(const Bytes &bytes, const char *expectedError = nullptr, uint64_t sliceOffset = 0, uint32_t expectedType = MH_DYLINKER) {
    FileMapping file;
    file.bytes = const_cast<uint8_t *>(bytes.data());
    file.size = bytes.size();
    Layout layout;
    std::string error;
    bool valid = parseLayout(file, layout, error, expectedType);
    file.bytes = MAP_FAILED; // The fixture owns these bytes; FileMapping must not unmap its vector.
    if (expectedError) {
        check(!valid, "malformed fixture rejected");
        if (error.find(expectedError) == std::string::npos) fprintf(stderr, "Unexpected error: %s\n", error.c_str());
        check(error.find(expectedError) != std::string::npos, expectedError);
    } else {
        check(valid, error.c_str());
        check(layout.sliceOffset == sliceOffset && layout.sliceSize == 3 * page, "selected architecture slice");
        check(layout.entryOffset == entryOffset && layout.mappedSize == 4 * page, "entry and mapped extent");
    }
}

static void parserTests() {
    parse(thin()); parse(thin(true));
    parse(dylibVariant(thin())); parse(dylibVariant(thin(true)));
    parse(universal(true, true, dylibVariant(thin())), nullptr, page);
    parse(universal(false), nullptr, page); parse(universal(true), nullptr, page);
    parse(universal(false, false), nullptr, page); parse(universal(true, false), nullptr, page);
    auto bad = thin(); bad.resize(4); parse(bad, "too small");
    bad = thin(); put<uint32_t>(bad, 0, 0); parse(bad, "not a 64-bit");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.ncmds = UINT32_MAX; }); parse(bad, "load command bounds");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.sizeofcmds = UINT32_MAX; }); parse(bad, "load command bounds");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.cputype = CPU_TYPE_X86_64; }); parse(bad, "little-endian arm64");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.cpusubtype = CPU_SUBTYPE_ARM64E; }); parse(bad, "little-endian arm64");
    bad = thin(); edit<mach_header_64>(bad, 0, [](auto &h) { h.filetype = MH_EXECUTE; }); parse(bad, "MH_DYLINKER");
    bad = thin(); edit<load_command>(bad, textCommand, [](auto &c) { c.cmdsize = 9; }); parse(bad, "command size");
    bad = thin(); edit<load_command>(bad, textCommand, [](auto &c) { c.cmdsize = 8; }); parse(bad, "Truncated segment");
    bad = thin(); edit<segment_command_64>(bad, textCommand, [](auto &s) { s.nsects = 1; }); parse(bad, "segment sections");
    bad = thin(); edit<segment_command_64>(bad, dataCommand, [](auto &s) { s.filesize = UINT64_MAX; }); parse(bad, "segment bounds");
    bad = thin(); edit<segment_command_64>(bad, dataCommand, [](auto &s) { s.vmaddr++; }); parse(bad, "not aligned");
    bad = thin(); edit<segment_command_64>(bad, dataCommand, [](auto &s) { s.vmaddr = preferred; }); parse(bad, "overlap");
    bad = thin(); edit<segment_command_64>(bad, dataCommand, [](auto &s) { s.fileoff++; }); parse(bad, "file offset");
    bad = thin(); edit<load_command>(bad, signatureCommand, [](auto &c) { c.cmd = LC_UUID; }); parse(bad, "embedded code signature");
    bad = thin(); edit<linkedit_data_command>(bad, signatureCommand, [](auto &c) { c.datasize = UINT32_MAX; }); parse(bad, "signature bounds");
    bad = thin(true); edit<entry_point_command>(bad, entryCommand, [](auto &c) { c.entryoff += page; }); parse(bad, "outside executable");
    bad = thin(true); edit<entry_point_command>(bad, entryCommand, [](auto &c) { c.entryoff++; }); parse(bad, "unaligned");
    bad = thin(); put<uint32_t>(bad, entryCommand + 12, 69); parse(bad, "Truncated thread state");
    bad = thin(); put<uint32_t>(bad, entryCommand + 12, 67); parse(bad, "Invalid or duplicate arm64 entry");
    bad = universal(false); put<uint32_t>(bad, 4, OSSwapInt32(UINT32_MAX)); parse(bad, "architecture table");
    bad = universal(true); edit<fat_arch_64>(bad, 8, [](auto &a) { a.cputype = OSSwapInt32(CPU_TYPE_X86_64); }); parse(bad, "no compatible");
    bad = universal(true); edit<fat_arch_64>(bad, 8, [](auto &a) { a.size = OSSwapInt64(UINT64_MAX); }); parse(bad, "slice bounds");
    bad = universal(false); edit<fat_arch>(bad, 8, [](auto &a) { a.offset = OSSwapInt32(page + 1); }); parse(bad, "slice bounds");
}

static void executableParserTests() {
    parse(executable(), nullptr, 0, MH_EXECUTE);
    parse(dylibVariant(executable()), nullptr, 0, MH_EXECUTE);
    parse(universal(true, true, dylibVariant(executable())), nullptr, page, MH_EXECUTE);
    parse(universal(false, true, executable()), nullptr, page, MH_EXECUTE);
    parse(universal(true, true, executable()), nullptr, page, MH_EXECUTE);
    parse(thin(), "MH_EXECUTE", 0, MH_EXECUTE);
    auto bad = executable();
    edit<mach_header_64>(bad, 0, [](auto &header) { header.flags &= ~MH_PIE; });
    parse(bad, "MH_PIE", 0, MH_EXECUTE);
    parse(dylibVariant(bad), "MH_PIE", 0, MH_EXECUTE);
    bad = executable();
    edit<build_version_command>(bad, buildCommand, [](auto &build) { build.platform = PLATFORM_IOS; });
    parse(bad, nullptr, 0, MH_EXECUTE);
    parse(dylibVariant(bad), nullptr, 0, MH_EXECUTE);
    for (uint32_t platform : {PLATFORM_MACOS, PLATFORM_TVOS, PLATFORM_WATCHOS, PLATFORM_MACCATALYST, PLATFORM_TVOSSIMULATOR, 0, 99}) {
        edit<build_version_command>(bad, buildCommand, [=](auto &build) { build.platform = platform; });
        parse(bad, "single iOS or iOS Simulator", 0, MH_EXECUTE);
    }
    // Legacy device binaries keep their original load command rather than acquiring LC_BUILD_VERSION.
    auto legacy = executable();
    const size_t legacyPagezero = buildCommand + sizeof(version_min_command);
    memmove(legacy.data() + legacyPagezero, legacy.data() + pagezeroCommand, sizeof(segment_command_64));
    version_min_command minimum = {LC_VERSION_MIN_IPHONEOS, sizeof(minimum), 0x70000, 0};
    put(legacy, buildCommand, minimum);
    edit<mach_header_64>(legacy, 0, [](auto &header) { header.sizeofcmds -= sizeof(build_version_command) - sizeof(version_min_command); });
    parse(legacy, nullptr, 0, MH_EXECUTE);
    parse(dylibVariant(legacy), nullptr, 0, MH_EXECUTE);
    for (uint32_t command : {LC_VERSION_MIN_MACOSX, LC_VERSION_MIN_TVOS, LC_VERSION_MIN_WATCHOS}) {
        bad = legacy;
        edit<load_command>(bad, buildCommand, [=](auto &load) { load.cmd = command; });
        parse(bad, "single iOS or iOS Simulator", 0, MH_EXECUTE);
    }
    bad = executable();
    edit<load_command>(bad, buildCommand, [](auto &command) { command.cmd = LC_VERSION_MIN_IPHONEOS; });
    parse(bad, "Invalid minimum OS", 0, MH_EXECUTE);
    for (auto bytes : {executable(), legacy}) {
        auto header = read<mach_header_64>(bytes.data());
        put(bytes, sizeof(header) + header.sizeofcmds, minimum);
        ++header.ncmds;
        header.sizeofcmds += sizeof(minimum);
        put(bytes, 0, header);
        parse(bytes, "single iOS or iOS Simulator", 0, MH_EXECUTE);
    }
    bad = executable();
    edit<load_command>(bad, buildCommand, [](auto &command) { command.cmd = LC_UUID; });
    parse(bad, "iOS Simulator", 0, MH_EXECUTE);
    bad = executable();
    edit<segment_command_64>(bad, pagezeroCommand, [](auto &segment) { segment.vmsize += page; });
    parse(bad, "overlapping __PAGEZERO", 0, MH_EXECUTE);
    bad = thin(); // A valid legacy thread entry must not run the program during dyld_sim preparation.
    size_t legacyBuildCommand = sizeof(mach_header_64) + read<mach_header_64>(bad.data()).sizeofcmds;
    build_version_command build = {LC_BUILD_VERSION, sizeof(build), PLATFORM_IOSSIMULATOR, 0x110000, 0x110000, 0};
    put(bad, legacyBuildCommand, build);
    edit<mach_header_64>(bad, 0, [](auto &header) {
        header.filetype = MH_EXECUTE;
        header.flags = MH_PIE;
        ++header.ncmds;
        header.sizeofcmds += sizeof(build_version_command);
    });
    parse(bad, "LC_MAIN", 0, MH_EXECUTE);
    parse(dylibVariant(bad), "LC_MAIN", 0, MH_EXECUTE);
}

static void mappingFailure(const Bytes &bytes, bool afterReservation, bool isExecutable = false) {
    char path[] = "/tmp/LiveSimBooter-test-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "create temporary fixture");
    check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "write fixture");
    close(fd);
    sourceMapping = MAP_FAILED; sourceFD = -1; signatureCalls = deallocations = fileUnmaps = 0;
    rejectSegmentMapping = afterReservation;
    LSBMappedImage image = {};
    char error[256];
    bool success = isExecutable ? LSBMapSimExecutable(path, &image, error, sizeof(error)) : LSBMapDyldSim(path, &image, error, sizeof(error));
    unlink(path);
    check(!success && error[0], "mapping failure reports an error");
    check(!image.header && !image.mappedSize && !image.slide && !image.entry, "failure resets output image");
    check(sourceFD >= 0 && ::fcntl(sourceFD, F_GETFD) == -1 && errno == EBADF, "failure closes source descriptor");
    check(fileUnmaps == 1, "failure unmaps source view exactly once");
    check(deallocations == (afterReservation ? 1 : 0), "failure releases reserved image exactly once");
    check(signatureCalls == (afterReservation ? 1 : 0), "signature registration occurs after parsing");
    if (afterReservation) {
        check(registeredSlice == static_cast<off_t>(page), "signature registration preserves universal slice offset");
        check(registeredSignature == 3 * page - 64, "signature blob offset remains relative to slice");
    }
    rejectSegmentMapping = false;
}

static void partialPageMapping(bool asDylib = false) {
    auto bytes = asDylib ? dylibVariant(thin()) : thin();
    edit<segment_command_64>(bytes, dataCommand, [](auto &s) { s.filesize = page - 16; });
    std::fill(bytes.begin() + page, bytes.begin() + 2 * page, 0xA5);
    parse(bytes);
    char path[] = "/tmp/LiveSimBooter-test-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "create partial-page fixture");
    check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "write partial-page fixture");
    close(fd);
    LSBMappedImage image = {};
    char error[256];
    bool mapped = LSBMapDyldSim(path, &image, error, sizeof(error));
    unlink(path);
    check(mapped, error);
    check(image.header && image.header->filetype == (asDylib ? MH_DYLIB : MH_DYLINKER), "dyld mapping retains signed header type");
    auto *data = reinterpret_cast<const uint8_t *>(image.header) + page;
    check(std::all_of(data, data + page - 16, [](uint8_t value) { return value == 0xA5; }), "mapped data retains file-backed bytes");
    check(std::all_of(data + page - 16, data + 2 * page, [](uint8_t value) { return value == 0; }), "partial and full zero-fill pages are zero");
    check(image.entry == reinterpret_cast<uintptr_t>(image.header) + entryOffset, "mapped entry resolves relative to actual base");
    check(image.slide == reinterpret_cast<uintptr_t>(image.header) - preferred, "mapped slide resolves from preferred base");
    int previousDeallocations = deallocations;
    LSBUnloadImage(&image);
    check(!image.header && !image.entry && deallocations == previousDeallocations + 1, "successful image unload resets and releases image");
    LSBUnloadImage(&image);
    check(deallocations == previousDeallocations + 1, "unload of cleared image is harmless");
}

static void executableMapping(const Bytes &bytes, uint64_t sliceOffset) {
    char path[] = "/tmp/LiveSimBooter-test-XXXXXX";
    int fd = mkstemp(path);
    check(fd >= 0, "create executable fixture");
    check(write(fd, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "write executable fixture");
    close(fd);
    sourceMapping = MAP_FAILED; sourceFD = -1; signatureCalls = deallocations = fileUnmaps = 0;
    LSBMappedImage image = {};
    char error[256];
    bool mapped = LSBMapSimExecutable(path, &image, error, sizeof(error));
    unlink(path);
    check(mapped, error);
    auto originalHeader = read<mach_header_64>(bytes.data() + sliceOffset);
    check(image.header && image.header->filetype == originalHeader.filetype, "mapped simulator executable retains its signed Mach-O header");
    check(image.mappedSize == 4 * page, "PAGEZERO is excluded from executable allocation");
    check(image.entry == reinterpret_cast<uintptr_t>(image.header) + entryOffset, "executable entry resolves relative to mapped header");
    check(image.slide == reinterpret_cast<uintptr_t>(image.header) - preferred, "executable slide resolves relative to preferred base");
    check(signatureCalls == 1 && registeredSlice == static_cast<off_t>(sliceOffset), "executable registers selected slice signature");
    check(registeredSignature == 3 * page - 64, "executable signature blob offset remains relative to slice");
    check(fileUnmaps == 1 && sourceFD >= 0 && ::fcntl(sourceFD, F_GETFD) == -1 && errno == EBADF, "executable closes source mapping and descriptor");
    LSBUnloadImage(&image);
    check(!image.header && !image.mappedSize && !image.slide && !image.entry && deallocations == 1, "executable unload releases and clears image");
}

static SimSyscallHelpers fakeHelpers = {};
static std::function<void(unsigned)> preparationAllocationHook;
void LSBPrepareHostPolicy() { if (preparationAllocationHook) preparationAllocationHook(0); }
static const mach_header *debuggerExecutable;
static std::string debuggerExecutablePath;
static bool checkedFinalThreadCount;
int LSBValidateHostThreadCount() { checkedFinalThreadCount = true; return 0; }
int LSBPrepareHostDebugger(const mach_header *executable, const char *executablePath) {
    debuggerExecutable = executable;
    debuggerExecutablePath = executablePath;
    if (preparationAllocationHook) preparationAllocationHook(2);
    return 0;
}
int LSBPrepareHostMainThread(lsb::MainThreadBootstrap *bootstrap) {
    *bootstrap = {0x1000, 0x2000};
    if (preparationAllocationHook) preparationAllocationHook(1);
    return 0;
}
static const mach_header *initializedHeader = nullptr;
static std::string initializedPath;
const SimSyscallHelpers *LSBGetSimSyscallHelpers() { return &fakeHelpers; }
bool LSBInitializeSimProcessInfo(const mach_header *header, const char *path) {
    initializedHeader = header; initializedPath = path; return true;
}

using FakeMain = int (*)(int, const char *const[], const char *const[], const char *const[]);
static unsigned entryCalls = 0, mainCalls = 0;
static mach_header fakeExecutable = {}, fakeDyld = {};
static int fakeMain(int, const char *const[], const char *const[], const char *const[]) { ++mainCalls; return 0; }
static FakeMain fakeEntry(int argc, const char *const argv[], const char *const envp[], const char *const apple[], const mach_header *executable,
                          const mach_header *dyld, uintptr_t slide, const SimSyscallHelpers *helpers, uintptr_t *startGlue) {
    ++entryCalls;
    auto *kernelArgs = reinterpret_cast<const uintptr_t *>(argv) - 2;
    check(kernelArgs[0] == reinterpret_cast<uintptr_t>(executable) && kernelArgs[1] == 2, "kernel-style prefix before argv");
    check(argc == 2 && std::string(argv[0]) == "/RuntimeRoot/bin/df" && std::string(argv[1]) == "arg" && !argv[2], "copied argv and terminator");
    check(envp == argv + argc + 1, "environment follows argv contiguously");
    check(std::string(envp[0]) == "KEEP=value" && std::string(envp[3]) == "DYLD_ROOT_PATH=/RuntimeRoot", "root override preserves other environment");
    check(std::string(envp[4]) == "DYLD_SHARED_CACHE_DIR=/RuntimeRoot/System/Library/Caches/com.apple.dyld", "cache directory override");
    size_t environmentCount = 0, diagnosticSettings = 0;
    for (; envp[environmentCount]; ++environmentCount) {
        std::string value = envp[environmentCount];
        if (value.rfind("DYLD_PRINT_", 0) == 0) {
            ++diagnosticSettings;
            check(value == "DYLD_PRINT_TO_STDERR=0" || value == "DYLD_PRINT_TO_FILE=/old.log", "explicit diagnostic settings are preserved");
        }
    }
    check(diagnosticSettings == 2 && environmentCount == 5, "no default simulator diagnostics are injected");
    check(std::string(envp[1]) == "DYLD_PRINT_TO_STDERR=0" && std::string(envp[2]) == "DYLD_PRINT_TO_FILE=/old.log", "diagnostic output is unchanged");
    check(apple == envp + environmentCount + 1, "apple vector follows expanded environment contiguously");
    unsigned executablePaths = 0, preservedApple = 0, mainStacks = 0, ptrMungeTokens = 0;
    for (const char *const *item = apple; *item; ++item) {
        std::string value = *item;
        check(value.rfind("executable_file=", 0) != 0, "host executable file identity is removed");
        if (value.rfind("executable_path=", 0) == 0) {
            ++executablePaths;
            check(value == "executable_path=/RuntimeRoot/bin/df", "apple executable path names mapped df");
        } else if (value.rfind("main_stack=", 0) == 0) {
            ++mainStacks;
            checkAndConsumeMainStack(*item);
        } else if (value.rfind("ptr_munge=", 0) == 0) {
            ++ptrMungeTokens;
            check(value.rfind("ptr_munge=0x", 0) == 0, "token includes the prefix required by Apple's pthread parser");
            char *end = nullptr;
            auto token = strtoull(*item + 10, &end, 16);
            check(token == freshPtrMungeToken && end && !*end, "simulator receives a fresh nonzero hexadecimal ptr_munge token");
            memset(const_cast<char *>(*item) + 10, 0, value.size() - 10);
            check(std::string(*item) == "ptr_munge=", "simulator can consume and erase its own token storage");
        } else {
            ++preservedApple;
            check(value == "stack_guard=0x1234", "unrelated apple value is preserved");
        }
    }
    check(executablePaths == 1 && preservedApple == 1, "apple has one replacement executable path and preserved values");
    check(mainStacks == 1, "all consumed or stale main_stack entries are replaced exactly once");
    check(ptrMungeTokens == 1 && randomCalls == 2, "all inherited tokens are replaced and zero random values are retried");
    check(executable == &fakeExecutable && dyld == &fakeDyld && slide == 42 && helpers == &fakeHelpers, "entry ABI image and helper arguments");
    check(initializedHeader == dyld && initializedPath == "/RuntimeRoot/usr/lib/dyld_sim", "process info initialized before entry");
    check(debuggerExecutable == executable && debuggerExecutablePath == "/RuntimeRoot/bin/df", "debugger metadata receives mapped executable and path");
    check(startGlue && *startGlue == 0, "start glue output initialized");
    *startGlue = 123;
    return fakeMain;
}

struct HandoffComplete {};
int LSBDetachHostCacheAndEnter(const LSBDyldSimLaunch *launch) {
    check(checkedFinalThreadCount, "native worker guard runs before destructive cache detachment");
    check(launch->thread.hostTSD == 0x1000 && launch->thread.simulatorTSD == 0x2000, "handoff receives prepared thread storage");
    // The real implementation never returns after entry. Model that boundary without detaching this test process's cache.
    uintptr_t startGlue = 0;
    auto main = launch->entry(launch->argc, launch->argv, launch->envp, launch->apple, launch->executable,
                              launch->dyld, launch->slide, launch->helpers, &startGlue);
    check(main == fakeMain && startGlue == 123, "handoff receives the returned main and start glue");
    throw HandoffComplete{};
}

struct TerminalStartupFixture {
    std::vector<std::string> arguments, environment, apple, expectedEnvironment, expectedApple;
    std::vector<std::pair<uintptr_t, size_t>> inputRanges;
    unsigned prepared = 0;

    explicit TerminalStartupFixture(bool large) {
        arguments = {"/RuntimeRoot/bin/fish", "-i", "-N", "-P", "-C",
                     "status job-control none; set -g fish_use_posix_spawn 1; set -g fish_greeting; function fish_prompt; printf 'fish> '; end"};
        environment = {"TERM=xterm-256color", "HOME=/RuntimeRoot/home/mobile", "PATH=/RuntimeRoot/bin:/RuntimeRoot/usr/bin",
                       "LSB_FAKE_PTY_STATE=/RuntimeRoot/.terminal-state", "DYLD_INSERT_LIBRARIES=/RuntimeRoot/usr/lib/libLiveSimSpawn.dylib",
                       "DYLD_ROOT_PATH=/old", "DYLD_SHARED_CACHE_DIR=/old-cache"};
        apple = {"executable_path=/app/LiveProcess", "executable_file=0x123,0x456", "stack_guard=0x1234",
                 "main_stack=0x100000,0x10000", "main_stack=", "ptr_munge=", "ptr_munge=0x1234"};
        if (large) {
            // Force repeated vector reallocations and both short-string and heap-backed storage.
            for (unsigned i = 0; i < 240; ++i) {
                arguments.emplace_back(i % 4 == 0 ? "" : std::string(i % 4 == 1 ? 15 : 257 + i, 'a' + i % 26));
                environment.emplace_back("FILL_" + std::to_string(i) + "=" + std::string(i % 4 == 0 ? 0 : 23 + i * 7, 'A' + i % 26));
                apple.emplace_back("extra_" + std::to_string(i) + "=" + std::string(i % 4 == 0 ? 0 : 17 + i * 5, '0' + i % 10));
            }
            environment.emplace_back("DYLD_ROOT_PATH=/second-old");
            environment.emplace_back("DYLD_SHARED_CACHE_DIR=/second-old-cache");
            apple.emplace_back("executable_path=/second-old-app");
            apple.emplace_back("executable_file=0x789,0xabc");
            apple.emplace_back("main_stack=0x180000,0x1000,0x170000,0x11000");
            apple.emplace_back("ptr_munge=0xffff");
        }
        for (const auto &item : environment)
            if (item.rfind("DYLD_ROOT_PATH=", 0) != 0 && item.rfind("DYLD_SHARED_CACHE_DIR=", 0) != 0) expectedEnvironment.push_back(item);
        expectedEnvironment.emplace_back("DYLD_ROOT_PATH=/RuntimeRoot");
        expectedEnvironment.emplace_back("DYLD_SHARED_CACHE_DIR=/RuntimeRoot/System/Library/Caches/com.apple.dyld");
        for (const auto &item : apple)
            if (item.rfind("executable_path=", 0) != 0 && item.rfind("executable_file=", 0) != 0 &&
                item.rfind("main_stack=", 0) != 0 && item.rfind("ptr_munge=", 0) != 0)
                expectedApple.push_back(item);
        expectedApple.emplace_back("executable_path=/RuntimeRoot/bin/fish");
        expectedApple.emplace_back(expectedMainStack());
        expectedApple.emplace_back("ptr_munge=0xa739c450e16b28df");
    }

    std::vector<const char *> pointers(std::vector<std::string> &strings) {
        std::vector<const char *> result;
        for (auto &item : strings) {
            result.push_back(item.data());
            inputRanges.emplace_back(reinterpret_cast<uintptr_t>(item.data()), item.size() + 1);
        }
        result.push_back(nullptr);
        return result;
    }

    void checkVector(const char *const actual[], const std::vector<std::string> &expected, const char *message) const {
        for (size_t i = 0; i < expected.size(); ++i) {
            check(actual[i] != nullptr, "startup vector has no premature null entry");
            check(strcmp(actual[i], expected[i].c_str()) == 0, message);
            uintptr_t pointer = reinterpret_cast<uintptr_t>(actual[i]);
            bool aliasesInput = false;
            for (const auto &range : inputRanges) aliasesInput |= pointer >= range.first && pointer - range.first < range.second;
            check(!aliasesInput, "startup strings own storage separate from every original vector");
        }
        check(actual[expected.size()] == nullptr, "startup vector has its final null terminator");
    }
};

static TerminalStartupFixture *terminalFixture;
static FakeMain terminalEntry(int argc, const char *const argv[], const char *const envp[], const char *const apple[], const mach_header *executable,
                              const mach_header *dyld, uintptr_t slide, const SimSyscallHelpers *helpers, uintptr_t *startGlue) {
    ++entryCalls;
    auto &fixture = *terminalFixture;
    check(fixture.prepared == 7, "all native preparation allocation stages completed before entry");
    check(argc == static_cast<int>(fixture.arguments.size()), "terminal argument count survives native preparation");
    const auto *kernelArgs = reinterpret_cast<const uintptr_t *>(argv) - 2;
    check(kernelArgs[0] == reinterpret_cast<uintptr_t>(executable) && kernelArgs[1] == static_cast<uintptr_t>(argc), "terminal kernel-style prefix");
    fixture.checkVector(argv, fixture.arguments, "terminal argument contents survive native allocation");
    auto reconstructedEnvironment = argv + kernelArgs[1] + 1;
    check(reconstructedEnvironment == envp, "dyld reconstructs the exact supplied environment pointer");
    fixture.checkVector(envp, fixture.expectedEnvironment, "terminal environment contents and override order survive native allocation");
    auto reconstructedApple = reconstructedEnvironment;
    while (*reconstructedApple) ++reconstructedApple;
    ++reconstructedApple;
    check(reconstructedApple == apple, "dyld finds the exact supplied apple vector after scanning environment");
    fixture.checkVector(apple, fixture.expectedApple, "terminal apple contents and replacement order survive native allocation");
    check(randomCalls == 2, "terminal startup retries zero before generating a fresh pointer token");
    size_t tokenIndex = fixture.expectedApple.size() - 1;
    checkAndConsumeMainStack(apple[tokenIndex - 1]);
    char *token = const_cast<char *>(apple[tokenIndex]);
    check(strncmp(token, "ptr_munge=0x", 12) == 0 && !apple[tokenIndex + 1], "fresh pointer token is the final apple entry");
    size_t tokenLength = strlen(token);
    memset(token + 10, 0, tokenLength - 10);
    check(strcmp(token, "ptr_munge=") == 0, "terminal pointer token remains writable for libpthread consumption");
    check(apple[tokenIndex - 1] && !apple[tokenIndex + 1], "token erasure preserves adjacent vector entries and final terminator");
    check(executable == &fakeExecutable && dyld == &fakeDyld && slide == 42 && helpers == &fakeHelpers, "terminal entry retains image and helper ABI");
    check(debuggerExecutable == executable && debuggerExecutablePath == "/RuntimeRoot/bin/fish", "terminal debugger preparation receives the original path");
    check(startGlue && *startGlue == 0, "terminal start glue is initialized");
    *startGlue = 123;
    return fakeMain;
}

static void terminalEntryTest(bool large) {
    // Each child starts before the parent's one permitted handoff; this is host-test isolation, not guest fork support.
    pid_t child = fork();
    check(child >= 0, "create isolated terminal startup test");
    if (child == 0) {
        unsigned initialChecks = checks;
        TerminalStartupFixture fixture(large);
        terminalFixture = &fixture;
        auto inputArguments = fixture.arguments, inputEnvironment = fixture.environment, inputApple = fixture.apple;
        auto argv = fixture.pointers(inputArguments), envp = fixture.pointers(inputEnvironment), apple = fixture.pointers(inputApple);
        preparationAllocationHook = [&](unsigned stage) {
            check(stage < 3 && fixture.prepared == (1u << stage) - 1, "native allocation hooks run in handoff order");
            fixture.prepared |= 1u << stage;
            if (stage == 0) {
                for (auto *strings : {&inputArguments, &inputEnvironment, &inputApple})
                    for (auto &item : *strings) std::fill(item.begin(), item.end(), '!');
            }
            std::vector<std::string> churn;
            for (unsigned i = 0; i < 512; ++i) churn.emplace_back((i * 37 + stage * 17) % 8192, 'A' + i % 26);
            check(churn.size() == 512 && !churn.back().empty(), "native preparation exercises allocator growth and release");
        };
        LSBMappedImage image = {&fakeDyld, 0, 42, reinterpret_cast<uintptr_t>(&terminalEntry)};
        bool handedOff = false;
        try {
            LSBEnterDyldSim(&image, &fakeExecutable, "/RuntimeRoot/bin/fish", "/RuntimeRoot/usr/lib/dyld_sim", "/RuntimeRoot",
                           static_cast<int>(inputArguments.size()), argv.data(), envp.data(), apple.data());
        } catch (const HandoffComplete &) { handedOff = true; }
        check(handedOff && entryCalls == 1 && !mainCalls, "terminal prepare reaches the destructive handoff boundary exactly once");
        printf("Terminal startup %s vector passed (%u checks).\n", large ? "large" : "short", checks - initialChecks);
        fflush(stdout);
        _exit(0);
    }
    int status = 0;
    check(waitpid(child, &status, 0) == child, "wait for isolated terminal startup test");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "terminal startup vector survives native preparation allocations");
}

static void entryTest() {
    LSBMappedImage image = {&fakeDyld, 0, 42, reinterpret_cast<uintptr_t>(&fakeEntry)};
    const char *argv[] = {"/RuntimeRoot/bin/df", "arg", nullptr};
    const char *envp[] = {"DYLD_ROOT_PATH=/old", "KEEP=value", "DYLD_SHARED_CACHE_DIR=/old-cache", "DYLD_PRINT_TO_STDERR=0",
                          "DYLD_PRINT_TO_FILE=/old.log", nullptr};
    const char *apple[] = {"executable_path=/app", "executable_file=0x123,0x456", "stack_guard=0x1234", "ptr_munge=", "ptr_munge=0x1234",
                           "main_stack=", "main_stack=0x1,0x2,0x3,0x4", nullptr};
    auto enter = [&] {
        return LSBEnterDyldSim(&image, &fakeExecutable, "/RuntimeRoot/bin/df", "/RuntimeRoot/usr/lib/dyld_sim", "/RuntimeRoot", 2, argv, envp, apple);
    };
    bool handedOff = false;
    try { enter(); } catch (const HandoffComplete &) { handedOff = true; }
    check(handedOff && entryCalls == 1 && mainCalls == 0, "entry is called without returning to host cleanup or invoking main");
    check(std::string(envp[3]) == "DYLD_PRINT_TO_STDERR=0", "simulator environment copy leaves host diagnostics untouched");
    check(std::string(envp[4]) == "DYLD_PRINT_TO_FILE=/old.log", "simulator logging leaves inherited host output path untouched");
    check(std::string(apple[3]) == "ptr_munge=" && std::string(apple[4]) == "ptr_munge=0x1234", "simulator token erasure leaves host apple data untouched");
    check(std::string(apple[5]) == "main_stack=" && std::string(apple[6]) == "main_stack=0x1,0x2,0x3,0x4", "stack erasure leaves host apple untouched");
    check(enter() == 1 && entryCalls == 1, "second entry is rejected");
}

static void offMainEntryTest() {
    pthread_t worker;
    int result = 0;
    int error = pthread_create(&worker, nullptr, [](void *context) -> void * {
        LSBMappedImage image = {&fakeDyld, 0, 42, reinterpret_cast<uintptr_t>(&fakeEntry)};
        const char *argv[] = {"/RuntimeRoot/bin/df", nullptr};
        *static_cast<int *>(context) = LSBEnterDyldSim(&image, &fakeExecutable, argv[0], "/RuntimeRoot/usr/lib/dyld_sim", "/RuntimeRoot",
                                                    1, argv, nullptr, nullptr);
        return nullptr;
    }, &result);
    check(!error, "create a native worker to exercise the main-thread handoff guard");
    check(!pthread_join(worker, nullptr), "join the rejected handoff worker");
    check(result == 1 && !initializedHeader && !entryCalls && !checkedFinalThreadCount, "off-main handoff rejects before initialization or detachment");
}

int main() {
    parserTests();
    executableParserTests();
    auto bad = thin(); put<uint32_t>(bad, 0, 0);
    mappingFailure(bad, false);
    mappingFailure(universal(true), true);
    partialPageMapping();
    partialPageMapping(true);
    mappingFailure(thin(), false, true);
    mappingFailure(universal(true, true, executable()), true, true);
    executableMapping(executable(), 0);
    executableMapping(universal(false, true, executable()), page);
    executableMapping(universal(true, true, executable()), page);
    executableMapping(dylibVariant(executable()), 0);
    executableMapping(universal(true, true, dylibVariant(executable())), page);
    mainStackBoundsTest();
    offMainEntryTest();
    terminalEntryTest(false);
    terminalEntryTest(true);
    entryTest();
    printf("DyldSimLoader host tests passed (%u checks). Signature authorization and device execution are not tested.\n", checks);
    return 0;
}
