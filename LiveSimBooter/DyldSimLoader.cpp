/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2020 Apple Inc. All rights reserved.
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

// Modified 2026-10-23: adapt simulator image mapping, validation and launch arguments for an iPhone app.
// Modified 2026-10-24: accept MH_DYLIB signing variants while retaining validation for each image's launch role.
// Modified 2026-10-24: refresh ptr_munge in writable startup data and prepare debugger metadata before host-cache removal.
// Mapping sequence based on Apple's prepareSim(), dyldMain.cpp:300-469 at fd8d0c4d52320ebf64db34f3cb280310d905c5ae.
// https://github.com/apple-oss-distributions/dyld/blob/fd8d0c4d52320ebf64db34f3cb280310d905c5ae/dyld/dyldMain.cpp

#include "DyldSimLoader.h"
#include "HostPreparation.h"
#include "SharedCacheHandoff.h"
#include "SimSyscallHelpers.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach/mach.h>
#include <mach-o/fat.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if __has_feature(ptrauth_calls)
#include <ptrauth.h>
#endif

namespace {

struct FileMapping {
    int fd = -1;
    void *bytes = MAP_FAILED;
    size_t size = 0;
    ~FileMapping() {
        if (bytes != MAP_FAILED) munmap(bytes, size);
        if (fd != -1) close(fd);
    }
};

struct Layout {
    uint64_t sliceOffset = 0;
    uint64_t sliceSize = 0;
    uint64_t preferredAddress = 0;
    uint64_t mappedSize = 0;
    uint64_t entryOffset = 0;
    linkedit_data_command signature = {};
    std::vector<segment_command_64> segments;
};

bool contains(uint64_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= size - offset;
}

template <typename T> T read(const uint8_t *bytes) {
    T value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

bool fail(std::string &error, const char *message) {
    error = message;
    return false;
}

bool systemFailure(std::string &error, const char *operation) {
    error = std::string(operation) + ": " + strerror(errno);
    return false;
}

bool selectSlice(const FileMapping &file, Layout &layout, std::string &error) {
    const auto *bytes = static_cast<const uint8_t *>(file.bytes);
    if (file.size < sizeof(mach_header_64)) return fail(error, "Image is too small for a Mach-O header");
    uint32_t magic = read<uint32_t>(bytes);
    layout.sliceSize = file.size;
    if (magic == MH_MAGIC_64) return true;
    bool fat64 = magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64;
    if (!fat64 && magic != FAT_MAGIC && magic != FAT_CIGAM) return fail(error, "Image is not a 64-bit Mach-O or universal binary");
    bool swapped = magic == FAT_CIGAM || magic == FAT_CIGAM_64;
    auto swap32 = [swapped](uint32_t value) { return swapped ? OSSwapInt32(value) : value; };
    auto swap64 = [swapped](uint64_t value) { return swapped ? OSSwapInt64(value) : value; };
    uint32_t count = swap32(read<fat_header>(bytes).nfat_arch);
    size_t archSize = fat64 ? sizeof(fat_arch_64) : sizeof(fat_arch);
    if (!contains(file.size, sizeof(fat_header), uint64_t(count) * archSize)) return fail(error, "Truncated universal architecture table");
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *entry = bytes + sizeof(fat_header) + i * archSize;
        uint32_t cpu, subtype, alignment;
        uint64_t offset, size;
        if (fat64) {
            auto arch = read<fat_arch_64>(entry);
            cpu = swap32(arch.cputype); subtype = swap32(arch.cpusubtype); alignment = swap32(arch.align);
            offset = swap64(arch.offset); size = swap64(arch.size);
        } else {
            auto arch = read<fat_arch>(entry);
            cpu = swap32(arch.cputype); subtype = swap32(arch.cpusubtype); alignment = swap32(arch.align);
            offset = swap32(arch.offset); size = swap32(arch.size);
        }
        // This host is built as arm64; an arm64e slice has a different ABI even on an arm64e-capable phone.
        if (cpu != CPU_TYPE_ARM64 || subtype != CPU_SUBTYPE_ARM64_ALL) continue;
        if (alignment > 63 || (offset & ((uint64_t(1) << alignment) - 1)) || !contains(file.size, offset, size))
            return fail(error, "Invalid arm64 slice bounds or alignment");
        if (offset < sizeof(fat_header) + uint64_t(count) * archSize || size < sizeof(mach_header_64))
            return fail(error, "Invalid arm64 slice header");
        layout.sliceOffset = offset;
        layout.sliceSize = size;
        return true;
    }
    return fail(error, "Image has no compatible arm64 slice");
}

bool parseLayout(const FileMapping &file, Layout &layout, std::string &error, uint32_t expectedType = MH_DYLINKER) {
    if (!selectSlice(file, layout, error)) return false;
    const auto *bytes = static_cast<const uint8_t *>(file.bytes) + layout.sliceOffset;
    auto header = read<mach_header_64>(bytes);
    if (header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 || header.cpusubtype != CPU_SUBTYPE_ARM64_ALL)
        return fail(error, "Image must contain a little-endian arm64 Mach-O");
    // The requested role determines validation even when the on-disk image has been re-signed as MH_DYLIB.
    if (header.filetype != expectedType && header.filetype != MH_DYLIB)
        return fail(error, expectedType == MH_EXECUTE ? "Simulator program must be MH_EXECUTE or MH_DYLIB" : "dyld_sim must be MH_DYLINKER or MH_DYLIB");
    if (expectedType == MH_EXECUTE && !(header.flags & MH_PIE))
        return fail(error, "Simulator program must use MH_PIE for allocation at a new address");
    if (!contains(layout.sliceSize, sizeof(header), header.sizeofcmds) || header.ncmds > header.sizeofcmds / sizeof(load_command))
        return fail(error, "Invalid Mach-O load command bounds");

    uint64_t cursor = sizeof(header), commandsEnd = cursor + header.sizeofcmds;
    uint64_t entryValue = 0;
    bool hasEntry = false, entryIsFileOffset = false, hasText = false, hasSimulatorPlatform = false;
    for (uint32_t i = 0; i < header.ncmds; ++i) {
        if (!contains(commandsEnd, cursor, sizeof(load_command))) return fail(error, "Truncated load command");
        auto command = read<load_command>(bytes + cursor);
        if (command.cmdsize < sizeof(command) || command.cmdsize % 8 || !contains(commandsEnd, cursor, command.cmdsize))
            return fail(error, "Invalid load command size");
        const uint8_t *data = bytes + cursor;
        if (command.cmd == LC_SEGMENT_64) {
            if (command.cmdsize < sizeof(segment_command_64)) return fail(error, "Truncated segment command");
            auto segment = read<segment_command_64>(data);
            if (!contains(command.cmdsize, sizeof(segment), uint64_t(segment.nsects) * sizeof(section_64)))
                return fail(error, "Truncated segment sections");
            if (segment.filesize > segment.vmsize || !contains(layout.sliceSize, segment.fileoff, segment.filesize)
                || segment.vmsize > UINT64_MAX - segment.vmaddr)
                return fail(error, "Invalid segment bounds");
            if ((segment.initprot & ~VM_PROT_ALL) || (segment.maxprot & ~VM_PROT_ALL) || (segment.initprot & ~segment.maxprot))
                return fail(error, "Invalid segment protections");
            if (segment.flags & (SG_HIGHVM | SG_PROTECTED_VERSION_1)) return fail(error, "Unsupported segment flags");
            if (strncmp(segment.segname, SEG_TEXT, sizeof(segment.segname)) == 0) {
                if (hasText || segment.fileoff != 0 || segment.filesize < commandsEnd || !(segment.initprot & VM_PROT_READ))
                    return fail(error, "Invalid __TEXT segment");
                hasText = true;
                layout.preferredAddress = segment.vmaddr;
            }
            if (segment.vmsize) layout.segments.push_back(segment);
        } else if (command.cmd == LC_BUILD_VERSION && expectedType == MH_EXECUTE) {
            if (command.cmdsize < sizeof(build_version_command)) return fail(error, "Truncated build version command");
            auto version = read<build_version_command>(data);
            if (!contains(command.cmdsize, sizeof(version), uint64_t(version.ntools) * sizeof(build_tool_version)))
                return fail(error, "Truncated build tool versions");
            if (version.platform != PLATFORM_IOSSIMULATOR || hasSimulatorPlatform)
                return fail(error, "Simulator program requires a single iOS Simulator platform declaration");
            hasSimulatorPlatform = true;
        } else if (command.cmd == LC_CODE_SIGNATURE) {
            if (command.cmdsize != sizeof(linkedit_data_command) || layout.signature.datasize) return fail(error, "Invalid code signature command");
            layout.signature = read<linkedit_data_command>(data);
            if (!layout.signature.datasize || !contains(layout.sliceSize, layout.signature.dataoff, layout.signature.datasize))
                return fail(error, "Invalid code signature bounds");
        } else if (command.cmd == LC_MAIN) {
            if (command.cmdsize != sizeof(entry_point_command) || hasEntry) return fail(error, "Invalid or duplicate entry point");
            entryValue = read<entry_point_command>(data).entryoff;
            hasEntry = true;
            entryIsFileOffset = true;
        } else if (command.cmd == LC_UNIXTHREAD) {
            size_t stateOffset = sizeof(load_command);
            while (stateOffset < command.cmdsize) {
                if (!contains(command.cmdsize, stateOffset, 8)) return fail(error, "Truncated thread state header");
                uint32_t flavor = read<uint32_t>(data + stateOffset), count = read<uint32_t>(data + stateOffset + 4);
                stateOffset += 8;
                if (!contains(command.cmdsize, stateOffset, uint64_t(count) * 4)) return fail(error, "Truncated thread state");
                // ARM_THREAD_STATE64 on-disk ABI: 68 uint32 words, PC at the 33rd uint64 (no pointer authentication).
                if (flavor == 6) {
                    if (count != 68 || hasEntry) return fail(error, "Invalid or duplicate arm64 entry state");
                    entryValue = read<uint64_t>(data + stateOffset + 32 * sizeof(uint64_t));
                    hasEntry = true;
                }
                stateOffset += size_t(count) * 4;
            }
        }
        cursor += command.cmdsize;
    }
    if (cursor != commandsEnd || !hasText || !hasEntry || !layout.signature.datasize)
        return fail(error, "Image requires valid load commands, __TEXT, an entry point and an embedded code signature");
    if (expectedType == MH_EXECUTE && !hasSimulatorPlatform)
        return fail(error, "Simulator program is missing an iOS Simulator LC_BUILD_VERSION");
    if (expectedType == MH_EXECUTE && !entryIsFileOffset)
        return fail(error, "Simulator program must use LC_MAIN; legacy LC_UNIXTHREAD may start the program during preparation");

    const uint64_t pageSize = static_cast<uint64_t>(getpagesize());
    std::sort(layout.segments.begin(), layout.segments.end(), [](const auto &a, const auto &b) { return a.vmaddr < b.vmaddr; });
    uint64_t previousEnd = layout.preferredAddress;
    bool entryInCode = false;
    for (const auto &segment : layout.segments) {
        if (segment.vmaddr < layout.preferredAddress) {
            if (strncmp(segment.segname, SEG_PAGEZERO, sizeof(segment.segname)) != 0 || segment.filesize || segment.initprot || segment.maxprot
                || segment.vmaddr + segment.vmsize > layout.preferredAddress)
                return fail(error, "Invalid or overlapping __PAGEZERO segment");
            continue; // The host already protects address zero; do not reserve the executable's entire __PAGEZERO span.
        }
        if (segment.vmaddr < previousEnd || segment.vmaddr % pageSize || segment.vmsize > UINT64_MAX - (pageSize - 1))
            return fail(error, "Segments overlap or are not aligned for the device page size");
        uint64_t roundedSize = (segment.vmsize + pageSize - 1) & ~(pageSize - 1);
        if (roundedSize > UINT64_MAX - segment.vmaddr) return fail(error, "Segment address overflow");
        if (segment.filesize && (layout.sliceOffset + segment.fileoff) % pageSize)
            return fail(error, "Segment file offset is not aligned for the device page size");
        if (segment.filesize < segment.vmsize && segment.filesize % pageSize && (segment.initprot & VM_PROT_EXECUTE))
            return fail(error, "Zero-fill inside a signed executable page is unsupported");
        previousEnd = segment.vmaddr + roundedSize;
        if ((segment.initprot & VM_PROT_EXECUTE) && segment.filesize) {
            uint64_t offset = entryIsFileOffset ? segment.fileoff : segment.vmaddr;
            if (entryValue >= offset && contains(segment.filesize, entryValue - offset, 4)) {
                layout.entryOffset = segment.vmaddr - layout.preferredAddress + entryValue - offset;
                entryInCode = true;
            }
        }
    }
    if (!entryInCode || entryValue % 4) return fail(error, "Entry point is outside executable file-backed segments or is unaligned");
    layout.mappedSize = previousEnd - layout.preferredAddress;
    if (!layout.mappedSize || layout.mappedSize > SIZE_MAX) return fail(error, "Invalid image virtual size");
    return true;
}

bool mapImage(const char *path, LSBMappedImage &image, std::string &error, uint32_t expectedType) {
    FileMapping file;
    file.fd = open(path, O_RDONLY | O_CLOEXEC);
    if (file.fd == -1) return systemFailure(error, "open(image)");
    struct stat info = {};
    if (fstat(file.fd, &info) == -1) return systemFailure(error, "fstat(image)");
    if (!S_ISREG(info.st_mode) || info.st_size <= 0 || uint64_t(info.st_size) > SIZE_MAX) return fail(error, "Image is not a nonempty regular file");
    file.size = static_cast<size_t>(info.st_size);
    file.bytes = mmap(nullptr, file.size, PROT_READ, MAP_PRIVATE, file.fd, 0);
    if (file.bytes == MAP_FAILED) return systemFailure(error, "mmap(image file)");
    Layout layout;
    if (!parseLayout(file, layout, error, expectedType)) return false;

    fsignatures_t signature = {};
    signature.fs_file_start = static_cast<off_t>(layout.sliceOffset);
    signature.fs_blob_start = reinterpret_cast<void *>(uintptr_t(layout.signature.dataoff));
    signature.fs_blob_size = layout.signature.datasize;
    if (fcntl(file.fd, F_ADDFILESIGS_RETURN, &signature) == -1) return systemFailure(error, "fcntl(F_ADDFILESIGS_RETURN)");
    // F_ADDFILESIGS_RETURN replaces fs_file_start with the covered length relative to the slice.
    if (signature.fs_file_start < 0 || uint64_t(signature.fs_file_start) < layout.signature.dataoff)
        return fail(error, "Code signature does not cover the image up to LC_CODE_SIGNATURE.dataoff");

    vm_address_t base = 0;
    kern_return_t result = vm_allocate(mach_task_self(), &base, static_cast<vm_size_t>(layout.mappedSize), VM_FLAGS_ANYWHERE);
    if (result != KERN_SUCCESS) { error = std::string("vm_allocate(image): ") + mach_error_string(result); return false; }
    image.header = reinterpret_cast<const mach_header *>(base);
    image.mappedSize = static_cast<size_t>(layout.mappedSize);
    // Keep unused gaps inaccessible; file mappings replace only pages inside this reservation.
    if (mprotect(reinterpret_cast<void *>(base), image.mappedSize, PROT_NONE) == -1) return systemFailure(error, "mprotect(image reservation)");
    const size_t pageSize = static_cast<size_t>(getpagesize());
    for (const auto &segment : layout.segments) {
        if (segment.vmaddr < layout.preferredAddress) continue;
        auto *address = reinterpret_cast<void *>(base + segment.vmaddr - layout.preferredAddress);
        if (segment.filesize) {
            off_t offset = static_cast<off_t>(layout.sliceOffset + segment.fileoff);
            void *mapped = mmap(address, static_cast<size_t>(segment.filesize), segment.initprot, MAP_FIXED | MAP_PRIVATE, file.fd, offset);
            if (mapped == MAP_FAILED) {
                error = std::string("mmap(") + std::string(segment.segname, strnlen(segment.segname, 16)) + "): " + strerror(errno);
                return false;
            }
            if (mapped != address) return fail(error, "mmap returned an unexpected segment address");
        }
        size_t filePages = (static_cast<size_t>(segment.filesize) + pageSize - 1) & ~(pageSize - 1);
        if (segment.vmsize > segment.filesize && segment.filesize % pageSize) {
            // mmap rounds to a page, which can include unrelated bytes after this segment in the file.
            auto *lastPage = static_cast<uint8_t *>(address) + filePages - pageSize;
            if (mprotect(lastPage, pageSize, PROT_READ | PROT_WRITE) == -1) return systemFailure(error, "mprotect(image partial page)");
            size_t tailSize = static_cast<size_t>(std::min<uint64_t>(filePages, segment.vmsize) - segment.filesize);
            memset(static_cast<uint8_t *>(address) + segment.filesize, 0, tailSize);
            if (mprotect(lastPage, pageSize, segment.initprot) == -1) return systemFailure(error, "mprotect(image partial page restore)");
        }
        if (segment.vmsize > filePages) {
            auto *zeroFill = static_cast<uint8_t *>(address) + filePages;
            if (mprotect(zeroFill, static_cast<size_t>(segment.vmsize - filePages), segment.initprot) == -1)
                return systemFailure(error, "mprotect(image zero-fill)");
        }
    }
    image.slide = base - layout.preferredAddress;
    image.entry = base + layout.entryOffset;
    return true;
}

bool mapFile(const char *path, LSBMappedImage *image, char *error, size_t errorSize, uint32_t expectedType) {
    if (!path || !image) {
        if (error && errorSize) snprintf(error, errorSize, "A path and output image are required");
        return false;
    }
    *image = {};
    std::string message;
    bool success = mapImage(path, *image, message, expectedType);
    if (!success) LSBUnloadImage(image);
    if (error && errorSize) snprintf(error, errorSize, "%s", message.c_str());
    return success;
}

} // namespace

bool LSBMapDyldSim(const char *path, LSBMappedImage *image, char *error, size_t errorSize) {
    return mapFile(path, image, error, errorSize, MH_DYLINKER);
}

bool LSBMapSimExecutable(const char *path, LSBMappedImage *image, char *error, size_t errorSize) {
    return mapFile(path, image, error, errorSize, MH_EXECUTE);
}

void LSBUnloadImage(LSBMappedImage *image) {
    if (!image) return;
    if (image->header) vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(image->header), image->mappedSize);
    *image = {};
}

int LSBEnterDyldSim(const LSBMappedImage *image, const mach_header *executable, const char *executablePath, const char *dyldPath, const char *rootPath,
                   int argc, const char *const argv[], const char *const envp[], const char *const apple[]) {
    if (!image || !image->entry || !executable || !executablePath || !dyldPath || !rootPath || argc < 1 || !argv) return 1;
    static bool entered = false;
    if (entered) { fprintf(stderr, "dyld_sim can only be entered once per process\n"); return 1; }
    if (!LSBInitializeSimProcessInfo(image->header, dyldPath)) {
        fprintf(stderr, "Cannot initialize simulator process info: %s\n", strerror(*LSBGetSimSyscallHelpers()->errnoAddress()));
        return 1;
    }

    // dyld_sim reconstructs KernelArgs at argv - 2 words and writes the executable header there.
    // Keep both strings and the contiguous kernel-style vector alive after prepare returns.
    static std::vector<std::string> arguments, environment, appleStrings;
    static std::vector<uintptr_t> kernelArgs;
    for (int i = 0; i < argc; ++i) arguments.emplace_back(argv[i]);
    for (auto entry = envp; entry && *entry; ++entry) {
        if (strncmp(*entry, "DYLD_ROOT_PATH=", 15) != 0 && strncmp(*entry, "DYLD_SHARED_CACHE_DIR=", 22) != 0) environment.emplace_back(*entry);
    }
    environment.emplace_back(std::string("DYLD_ROOT_PATH=") + rootPath);
    environment.emplace_back(std::string("DYLD_SHARED_CACHE_DIR=") + rootPath + "/System/Library/Caches/com.apple.dyld");
    for (auto entry = apple; entry && *entry; ++entry) {
        // executable_file identifies the host by FSID/inode and takes precedence over executable_path inside dyld.
        if (strncmp(*entry, "executable_path=", 16) != 0 && strncmp(*entry, "executable_file=", 16) != 0 && strncmp(*entry, "ptr_munge=", 10) != 0)
            appleStrings.emplace_back(*entry);
    }
    appleStrings.emplace_back(std::string("executable_path=") + executablePath);
    // Native libpthread has already consumed the kernel's token before main. The simulator needs its own nonzero secret.
    uintptr_t ptrMungeToken = 0;
    do { arc4random_buf(&ptrMungeToken, sizeof(ptrMungeToken)); } while (!ptrMungeToken);
    char ptrMungeEntry[sizeof("ptr_munge=0x") + 2 * sizeof(ptrMungeToken)];
    snprintf(ptrMungeEntry, sizeof(ptrMungeEntry), "ptr_munge=0x%llx", static_cast<unsigned long long>(ptrMungeToken));
    appleStrings.emplace_back(ptrMungeEntry);
    kernelArgs.push_back(reinterpret_cast<uintptr_t>(executable));
    kernelArgs.push_back(static_cast<uintptr_t>(argc));
    auto append = [](std::vector<std::string> &strings) {
        // libpthread erases the ptr_munge value after reading it, so expose owned, writable string storage.
        for (auto &string : strings) kernelArgs.push_back(reinterpret_cast<uintptr_t>(string.data()));
        kernelArgs.push_back(0);
    };
    append(arguments);
    size_t envOffset = kernelArgs.size();
    append(environment);
    size_t appleOffset = kernelArgs.size();
    append(appleStrings);

    using Entry = LSBDyldSimLaunch::Entry;
    auto entry = reinterpret_cast<Entry>(image->entry);
#if __has_feature(ptrauth_calls)
    entry = reinterpret_cast<Entry>(ptrauth_sign_unauthenticated(reinterpret_cast<void *>(image->entry), ptrauth_key_function_pointer, 0));
#endif
    auto simArgv = reinterpret_cast<const char *const *>(kernelArgs.data() + 2);
    auto simEnvp = reinterpret_cast<const char *const *>(kernelArgs.data() + envOffset);
    auto simApple = reinterpret_cast<const char *const *>(kernelArgs.data() + appleOffset);
    // Capture the real AMFI result and materialize arguments before the cache disappears.
    LSBPrepareHostPolicy();
    lsb::MainThreadBootstrap thread = {};
    int threadError = LSBPrepareHostMainThread(&thread);
    if (threadError) {
        fprintf(stderr, "Cannot prepare simulator main thread: %s\n", strerror(threadError));
        return 1;
    }
    int debuggerError = LSBPrepareHostDebugger(executable, executablePath);
    if (debuggerError) {
        fprintf(stderr, "Cannot prepare debugger image metadata: %s\n", strerror(debuggerError));
        return 1;
    }
    const SimSyscallHelpers *helpers = LSBGetSimSyscallHelpers();
    LSBDyldSimLaunch launch = {entry, argc, simArgv, simEnvp, simApple, executable, image->header, image->slide, helpers, thread};
    entered = true;
    fprintf(stderr, "Detaching host cache, then entering dyld_sim at %p (header %p, slide 0x%llx)\n", reinterpret_cast<void *>(image->entry), image->header,
            static_cast<unsigned long long>(image->slide));
    fflush(stderr);
    int detachError = LSBDetachHostCacheAndEnter(&launch);
    // The handoff returns only when detachment failed and host libraries are still mapped.
    fprintf(stderr, "Host shared-cache detach failed: %s\n", strerror(detachError));
    return 1;
}
