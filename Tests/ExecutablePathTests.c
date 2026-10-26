#include "../LiveProcess/ExecutablePath.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void join(char output[PATH_MAX], const char *directory, const char *name) {
    int length = snprintf(output, PATH_MAX, "%s/%s", directory, name);
    assert(length > 0 && length < PATH_MAX);
}

static void makeFile(const char *path) {
    int file = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(file >= 0 && !close(file));
}

static void resolves(const char *root, const char *path, const char *expected) {
    char result[PATH_MAX];
    assert(!LSBResolveGuestExecutable(root, path, result) && !strcmp(result, expected));
}

static void fails(const char *root, const char *path, int expected) {
    char result[PATH_MAX] = "unchanged";
    int error = LSBResolveGuestExecutable(root, path, result);
    assert((expected ? error == expected : error != 0) && !strcmp(result, "unchanged"));
}

int main(void) {
    char temporary[] = "/tmp/LiveSimBooter-executable-path.XXXXXX", base[PATH_MAX];
    assert(mkdtemp(temporary) && realpath(temporary, base));
    char root[PATH_MAX], bin[PATH_MAX], executable[PATH_MAX], alias[PATH_MAX], aliasedExecutable[PATH_MAX];
    join(root, base, "RuntimeRoot");
    join(bin, root, "bin");
    join(executable, bin, "df");
    join(alias, base, "root-alias");
    assert(!mkdir(root, 0700) && !mkdir(bin, 0700) && !symlink(root, alias));
    makeFile(executable);
    resolves(root, "/bin/df", executable);
    resolves(root, executable, executable);
    join(aliasedExecutable, alias, "bin/df");
    resolves(root, aliasedExecutable, executable);
    resolves(alias, aliasedExecutable, executable);
    // macOS /tmp -> /private/tmp reproduces iOS's /var -> /private/var physical prefix mismatch.
    join(aliasedExecutable, temporary, "RuntimeRoot/bin/df");
    resolves(root, aliasedExecutable, executable);

    char outside[PATH_MAX], escape[PATH_MAX], internal[PATH_MAX];
    join(outside, base, "outside");
    join(escape, bin, "escape");
    join(internal, bin, "internal");
    makeFile(outside);
    assert(!symlink(outside, escape) && !symlink(executable, internal));
    resolves(root, "/bin/internal", executable);
    fails(root, "/bin/escape", EACCES);
    fails(root, escape, EACCES);
    join(aliasedExecutable, alias, "bin/escape");
    fails(root, aliasedExecutable, 0);
    fails(root, outside, 0);
    fails(root, root, EACCES);
    fails(root, alias, EACCES);

    int originalCWD = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(originalCWD >= 0 && !chdir(bin));
    resolves(root, "df", executable);
    resolves(root, "../bin/df", executable);
    assert(!chdir(base));
    fails(root, "outside", EACCES);
    assert(!fchdir(originalCWD) && !close(originalCWD));

    // An existing native executable must still resolve to the separate guest file, never to host /usr/bin/false.
    char usr[PATH_MAX], usrBin[PATH_MAX], guestFalse[PATH_MAX];
    join(usr, root, "usr");
    join(usrBin, usr, "bin");
    join(guestFalse, usrBin, "false");
    assert(!mkdir(usr, 0700) && !mkdir(usrBin, 0700));
    makeFile(guestFalse);
    resolves(root, "/usr/bin/false", guestFalse);
    char missing[PATH_MAX];
    join(missing, root, "bin/missing");
    fails(root, missing, ENOENT);
    fails(root, "/bin/missing", ENOENT);
    fails(NULL, "/bin/df", EINVAL);
    fails("relative", "/bin/df", EINVAL);
    fails(root, NULL, EINVAL);
    fails(root, "", EINVAL);
    fails("/", "/bin/df", EINVAL);
    fails(executable, "/bin/df", ENOTDIR);
    assert(LSBResolveGuestExecutable(root, "/bin/df", NULL) == EINVAL);
    char oversized[PATH_MAX + 1];
    memset(oversized, 'x', sizeof(oversized));
    oversized[0] = '/'; oversized[PATH_MAX] = 0;
    fails(root, oversized, ENAMETOOLONG);
    oversized[PATH_MAX - 1] = 0;
    fails(root, oversized, ENAMETOOLONG);

    assert(!unlink(guestFalse) && !rmdir(usrBin) && !rmdir(usr));
    assert(!unlink(internal) && !unlink(escape) && !unlink(outside) && !unlink(executable));
    assert(!unlink(alias) && !rmdir(bin) && !rmdir(root) && !rmdir(base));
    puts("ExecutablePath tests passed");
    return 0;
}
