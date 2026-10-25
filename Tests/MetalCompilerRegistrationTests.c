#include "../Guest/MetalCompilerRegistration.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned registrations;
static uint64_t registeredFlags;
static char registeredPath[PATH_MAX];

static void recordRegistration(const char *path, uint64_t flags) {
    ++registrations;
    registeredFlags = flags;
    assert(strlen(path) < sizeof(registeredPath));
    strcpy(registeredPath, path);
}

#define LSB_METAL_COMPILER_REGISTRATION_TESTING
#define xpc_add_bundle recordRegistration
#include "../Guest/MetalCompilerRegistration.c"
#undef xpc_add_bundle

static void join(char output[PATH_MAX], const char *parent, const char *suffix) {
    assert(!appendPath(output, parent, suffix));
}

static void makeDirectory(const char *path) {
    char copy[PATH_MAX];
    assert(strlen(path) < sizeof(copy));
    strcpy(copy, path);
    for (char *separator = copy + 1; *separator; ++separator) {
        if (*separator != '/') continue;
        *separator = 0;
        assert(!mkdir(copy, 0700) || errno == EEXIST);
        *separator = '/';
    }
    assert(!mkdir(copy, 0700) || errno == EEXIST);
}

static void makeFile(const char *path) {
    int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0700);
    assert(descriptor >= 0 && write(descriptor, "fixture", 7) == 7 && !close(descriptor));
}

static void removeTree(char *path) {
    char *paths[] = {path, NULL};
    FTS *tree = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    assert(tree);
    FTSENT *entry;
    while ((entry = fts_read(tree))) {
        if (entry->fts_info == FTS_D) continue;
        assert(entry->fts_info != FTS_ERR && entry->fts_info != FTS_DNR && entry->fts_info != FTS_NS);
        assert(!(entry->fts_info == FTS_DP ? rmdir(entry->fts_accpath) : unlink(entry->fts_accpath)));
    }
    assert(!fts_close(tree));
}

static void expect(const char *enabled, const char *root, const char *candidate, int error, bool shouldRequest) {
    unsigned previous = registrations;
    bool requested = true;
    assert(LSBRegisterMetalCompilerForExecutable(enabled, root, candidate, &requested) == error);
    assert(requested == shouldRequest && registrations == previous + shouldRequest);
    if (requested) assert(registeredFlags == 2);
}

int main(void) {
    char temporary[] = "/tmp/LiveSimBooter-metal-compiler.XXXXXX", canonical[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, canonical));
    char root[PATH_MAX], directory[PATH_MAX], executable[PATH_MAX], bundle[PATH_MAX], info[PATH_MAX], compiler[PATH_MAX];
    join(root, canonical, "RuntimeRoot");
    join(directory, root, "usr/libexec"); makeDirectory(directory);
    join(executable, directory, "backboardd"); makeFile(executable);
    join(bundle, root, "System/Library/Frameworks/Metal.framework/XPCServices/MTLCompilerService.xpc"); makeDirectory(bundle);
    join(info, bundle, "Info.plist"); makeFile(info);
    join(compiler, bundle, "MTLCompilerService"); makeFile(compiler);

    // Opt-in is exact and does not inspect paths when disabled.
    const char *disabled[] = {NULL, "", "0", "true", "01", "1 "};
    for (unsigned i = 0; i < sizeof(disabled) / sizeof(*disabled); ++i) expect(disabled[i], NULL, NULL, 0, false);
    expect("1", NULL, executable, EINVAL, false);
    expect("1", "/", executable, EINVAL, false);
    expect("1", "RuntimeRoot", executable, EINVAL, false);
    expect("1", root, "usr/libexec/backboardd", EINVAL, false);
    assert(LSBRegisterMetalCompilerForExecutable("1", root, executable, NULL) == EINVAL && !registrations);

    expect("1", root, executable, 0, true);
    assert(!strcmp(registeredPath, bundle));
    char alias[PATH_MAX], candidate[PATH_MAX];
    join(alias, canonical, "runtime-alias"); assert(!symlink(root, alias));
    join(candidate, alias, "usr/libexec/../libexec/backboardd");
    expect("1", alias, candidate, 0, true);
    assert(!strcmp(registeredPath, bundle));

    // Inherited options never register compiler, proxy, launchd, unrelated, or merely prefix-matching executables.
    expect("1", root, compiler, 0, false);
    const char *otherRoles[] = {"launchd_sim", "xpcproxy_sim", "backboardd-other", "df"};
    for (unsigned i = 0; i < sizeof(otherRoles) / sizeof(*otherRoles); ++i) {
        join(candidate, directory, otherRoles[i]); makeFile(candidate);
        expect("1", root, candidate, 0, false);
    }
    char outside[PATH_MAX], outsideFile[PATH_MAX];
    join(outside, canonical, "RuntimeRoot-other"); makeDirectory(outside);
    join(outsideFile, outside, "backboardd"); makeFile(outsideFile);
    expect("1", root, outsideFile, 0, false);
    assert(!unlink(executable) && !symlink(outsideFile, executable));
    expect("1", root, executable, 0, false);
    assert(!unlink(executable)); makeFile(executable);

    // A bundle symlink escaping the canonical root is rejected even if its target contains plausible files.
    char moved[PATH_MAX];
    join(moved, outside, "MTLCompilerService.xpc");
    assert(!rename(bundle, moved) && !symlink(moved, bundle));
    expect("1", root, executable, EPERM, false);
    assert(!unlink(bundle));
    expect("1", root, executable, ENOENT, false);
    assert(!rename(moved, bundle));

    // Required files are regular, readable and nonempty; a final symlink or special file is not followed or blocked on.
    assert(!unlink(info)); expect("1", root, executable, ENOENT, false);
    assert(!symlink(outsideFile, info)); expect("1", root, executable, ELOOP, false);
    assert(!unlink(info) && !mkdir(info, 0700)); expect("1", root, executable, EINVAL, false);
    assert(!rmdir(info) && !mkfifo(info, 0600)); expect("1", root, executable, EINVAL, false);
    assert(!unlink(info)); makeFile(info);
    assert(!truncate(compiler, 0)); expect("1", root, executable, EINVAL, false);
    makeFile(compiler);
    assert(!unlink(compiler)); expect("1", root, executable, ENOENT, false);
    assert(!symlink(outsideFile, compiler)); expect("1", root, executable, ELOOP, false);
    assert(!unlink(compiler)); makeFile(compiler);
    expect("1", root, executable, 0, true);
    assert(registrations == 3);
    removeTree(canonical);
    puts("MetalCompilerRegistration: opt-in, canonical executable roles, contained bundle, regular files and exact registration passed");
    return 0;
}
