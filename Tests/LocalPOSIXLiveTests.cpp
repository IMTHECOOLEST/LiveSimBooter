// Harmless real-kernel smoke checks. These never detach the host shared cache or enter simulator code.
#include "../LiveSimBooter/LocalPOSIX.h"
#include "../LiveSimBooter/LocalRuntime.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace {

void testFilesAndMappings() {
    int fd = lsb::open("/bin/df", O_RDONLY);
    assert(fd >= 0);
    char bytes[4];
    assert(lsb::pread(fd, bytes, sizeof(bytes), 0) == sizeof(bytes));
    struct stat actual, expected;
    assert(lsb::fstat(fd, &actual) == 0 && ::fstat(fd, &expected) == 0);
    assert(actual.st_size == expected.st_size && actual.st_ino == expected.st_ino);
    assert(lsb::stat("/bin/df", &actual) == 0 && actual.st_ino == expected.st_ino);

    void *mapped = lsb::mmap(nullptr, 16384, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(mapped != MAP_FAILED && std::memcmp(mapped, bytes, sizeof(bytes)) == 0);
    char path[PATH_MAX];
    assert(lsb::fcntl(fd, F_GETPATH, path) == 0 && std::strcmp(path, "/bin/df") == 0);
    int pathSize = lsb::proc_regionfilename(getpid(), reinterpret_cast<uint64_t>(mapped), path, sizeof(path));
    assert(pathSize == 7 && std::strcmp(path, "/bin/df") == 0);
    assert(lsb::madvise(mapped, 16384, MADV_NORMAL) == 0);
    assert(lsb::munmap(mapped, 16384) == 0);
    assert(lsb::mmap(nullptr, 16384, PROT_READ, MAP_PRIVATE, fd, 1) == MAP_FAILED && *lsb::errnoAddress() == EINVAL);
    assert(lsb::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && (lsb::fcntl(fd, F_GETFD) & FD_CLOEXEC));
    assert(lsb::close(fd) == 0);

    struct statfs filesystem;
    assert(::statfs("/bin/df", &filesystem) == 0);
    assert(lsb::fsgetpath(path, sizeof(path), &filesystem.f_fsid, expected.st_ino) == 8);
    assert(std::strcmp(path, "/bin/df") == 0);
    assert(lsb::open("/no/such/lsb/path", O_RDONLY) == -1 && *lsb::errnoAddress() == ENOENT);
}

void testPathsAndDirectories() {
    char path[PATH_MAX], nativePath[PATH_MAX];
    assert(lsb::realpath("/tmp", path) == path && ::realpath("/tmp", nativePath) == nativePath);
    assert(std::strcmp(path, nativePath) == 0);
    assert(lsb::getcwd(path, sizeof(path)) && ::getcwd(nativePath, sizeof(nativePath)));
    assert(std::strcmp(path, nativePath) == 0);

    DIR *directory = lsb::opendir("/bin");
    assert(directory);
    bool found = false;
    dirent item;
    dirent *output;
    for (;;) {
        assert(lsb::readdir_r(directory, &item, &output) == 0);
        if (!output) break;
        if (std::strcmp(item.d_name, "df") == 0) found = true;
    }
    assert(found && lsb::readdir_r(directory, &item, &output) == 0 && !output);
    assert(lsb::closedir(directory) == 0);

    attrlist attributes = {};
    attributes.bitmapcount = ATTR_BIT_MAP_COUNT;
    attributes.commonattr = ATTR_CMN_NAME;
    char attrBuffer[4096];
    assert(lsb::getattrlist("/bin/df", &attributes, attrBuffer, sizeof(attrBuffer), 0) == 0);
    attributes.commonattr |= ATTR_CMN_RETURNED_ATTRS;
    int fd = lsb::open("/bin", O_RDONLY | O_DIRECTORY);
    assert(fd >= 0);
    assert(lsb::getattrlistbulk(fd, &attributes, attrBuffer, sizeof(attrBuffer), 0) > 0);
    assert(lsb::close(fd) == 0);
}

void testQueriesAndWrite() {
    char actual[128], expected[128];
    size_t size = sizeof(actual), expectedSize = sizeof(expected);
    assert(lsb::sysctlbyname("kern.osrelease", actual, &size, nullptr, 0) == 0);
    assert(::sysctlbyname("kern.osrelease", expected, &expectedSize, nullptr, 0) == 0);
    assert(size == expectedSize && std::memcmp(actual, expected, size) == 0);
    assert(lsb::getfsstat(nullptr, 0, MNT_NOWAIT) > 0);
    assert(lsb::getpid() == ::getpid() && lsb::issetugid() == ::issetugid());

    int descriptors[2];
    assert(::pipe(descriptors) == 0);
    assert(lsb::write(descriptors[1], "test", 4) == 4);
    int available = 0;
    assert(lsb::ioctl(descriptors[0], FIONREAD, &available) == 0 && available == 4);
    assert(::read(descriptors[0], actual, sizeof(actual)) == 4 && std::memcmp(actual, "test", 4) == 0);
    assert(lsb::close(descriptors[0]) == 0 && lsb::close(descriptors[1]) == 0);
}

} // namespace

int main() {
    testFilesAndMappings();
    testPathsAndDirectories();
    testQueriesAndWrite();
    std::puts("LocalPOSIX live ARM64 tests passed");
}
