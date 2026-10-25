#include "../LiveProcess/NativeProbeLoader.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void join(char output[PATH_MAX], const char *directory, const char *name) {
    int count = snprintf(output, PATH_MAX, "%s/%s", directory, name);
    assert(count > 0 && count < PATH_MAX);
}

static void writeFile(const char *path, const char *contents) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, contents, strlen(contents)) == (ssize_t)strlen(contents) && !close(fd));
}

static void copyFile(const char *source, const char *destination) {
    int input = open(source, O_RDONLY), output = open(destination, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(input >= 0 && output >= 0);
    char bytes[16384];
    ssize_t count;
    while ((count = read(input, bytes, sizeof(bytes))) > 0) {
        ssize_t offset = 0;
        while (offset < count) {
            ssize_t written = write(output, bytes + offset, (size_t)(count - offset));
            assert(written > 0);
            offset += written;
        }
    }
    assert(!count && !close(input) && !close(output));
}

static void testBundle(const char *root, const char *name, const char *fixture, bool missingSymbol) {
    char bundle[PATH_MAX], executable[PATH_MAX], frameworks[PATH_MAX], native[PATH_MAX], library[PATH_MAX];
    join(bundle, root, name);
    assert(!mkdir(bundle, 0700));
    join(executable, bundle, "LiveProcess");
    writeFile(executable, "executable path anchor");
    join(frameworks, bundle, "Frameworks");
    join(native, frameworks, "NativeMetal");
    join(library, native, "MetalNativeProbe.dylib");
    char resolvedFolder[PATH_MAX], resolvedLibrary[PATH_MAX], detail[1024];
    LSBNativeProbe probe = {0};
    assert(LSBNativeProbePrepareForExecutable(executable, &probe, detail, sizeof(detail)) == ENOENT && !probe.entry);
    assert(strstr(detail, "Frameworks/NativeMetal"));
    assert(!mkdir(frameworks, 0700) && !mkdir(native, 0700));
    assert(LSBNativeProbeResolvePaths(executable, resolvedFolder, resolvedLibrary) == ENOENT);
    writeFile(library, "not a Mach-O image");
    assert(LSBNativeProbePrepareForExecutable(executable, &probe, detail, sizeof(detail)) == ENOEXEC && !probe.entry);
    assert(strstr(detail, "dlopen") && !unlink(library));

    char outside[PATH_MAX];
    join(outside, root, "outside.dylib");
    assert(!symlink(outside, library));
    assert(LSBNativeProbeResolvePaths(executable, resolvedFolder, resolvedLibrary) == EACCES);
    assert(!unlink(library) && !rmdir(native));
    assert(!symlink(root, native));
    assert(LSBNativeProbeResolvePaths(executable, resolvedFolder, resolvedLibrary) == EACCES);
    assert(!unlink(native) && !mkdir(native, 0700));

    copyFile(fixture, library);
    if (missingSymbol) {
        assert(LSBNativeProbePrepareForExecutable(executable, &probe, detail, sizeof(detail)) == ENOEXEC);
        assert(probe.handle && !probe.entry && strstr(detail, "LSBMetalNativeProbe"));
        assert(LSBNativeProbeRun(&probe) == EINVAL);
    } else {
        assert(!LSBNativeProbeResolvePaths(executable, resolvedFolder, resolvedLibrary));
        assert(!LSBNativeProbePrepareForExecutable(executable, &probe, detail, sizeof(detail)) && !detail[0]);
        assert(probe.handle && probe.entry && !strcmp(probe.folder, resolvedFolder));
        assert(!LSBNativeProbeRun(&probe));
        char resultPath[PATH_MAX], result[PATH_MAX] = {0}, errorPath[PATH_MAX];
        join(resultPath, native, "probe-result");
        int fd = open(resultPath, O_RDONLY);
        assert(fd >= 0 && read(fd, result, sizeof(result) - 1) == (ssize_t)strlen(probe.folder) && !close(fd));
        assert(!strcmp(result, probe.folder));
        join(errorPath, native, "return-error");
        writeFile(errorPath, "");
        assert(LSBNativeProbeRun(&probe) == 37);
        assert(!unlink(errorPath) && !unlink(resultPath));
    }
    // The image stays loaded until process exit; removing the test's directory entries does not unload it.
    assert(!unlink(library) && !rmdir(native) && !rmdir(frameworks) && !unlink(executable) && !rmdir(bundle));
}

int main(int argc, char *argv[]) {
    assert(argc == 3);
    char root[] = "/tmp/LiveSimBooter-native-probe.XXXXXX", outside[PATH_MAX], folder[PATH_MAX], library[PATH_MAX];
    assert(mkdtemp(root));
    join(outside, root, "outside.dylib");
    copyFile(argv[1], outside);
    assert(LSBNativeProbeResolvePaths(NULL, folder, library) == EINVAL);
    assert(LSBNativeProbeResolvePaths("relative/LiveProcess", folder, library) == EINVAL);
    assert(LSBNativeProbeRun(NULL) == EINVAL);
    LSBNativeProbe empty = {0};
    assert(LSBNativeProbeRun(&empty) == EINVAL);
    testBundle(root, "Good.appex", argv[1], false);
    testBundle(root, "MissingSymbol.appex", argv[2], true);
    assert(!unlink(outside) && !rmdir(root));
    puts("NativeProbeLoader: fixed paths, missing build folder, symlink confinement, backend ABI and errors passed");
    return 0;
}
