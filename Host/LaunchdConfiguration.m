#import <Foundation/Foundation.h>
#include "LaunchdConfiguration.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int openDirectory(int parent, const char *name) {
    if (mkdirat(parent, name, 0700) && errno != EEXIST) return -1;
    return openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

static int writeConfiguration(int directory, NSData *data) {
    const char *name = "LiveSimBooter-launchd-bootstrap.plist";
    struct stat status;
    if (!fstatat(directory, name, &status, AT_SYMLINK_NOFOLLOW)) {
        if (!S_ISREG(status.st_mode)) return S_ISLNK(status.st_mode) ? ELOOP : EINVAL;
    } else if (errno != ENOENT) return errno;
    char temporary[80];
    int length = snprintf(temporary, sizeof(temporary), ".launchd-bootstrap.%s", NSUUID.UUID.UUIDString.UTF8String);
    if (length < 0 || (size_t)length >= sizeof(temporary)) return ENAMETOOLONG;
    int fd = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    int error = 0;
    const unsigned char *bytes = data.bytes;
    size_t remaining = data.length;
    while (remaining) {
        ssize_t count = write(fd, bytes, remaining);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        bytes += count;
        remaining -= (size_t)count;
    }
    if (close(fd) && !error) error = errno;
    if (!error && renameat(directory, temporary, directory, name)) error = errno;
    if (error) unlinkat(directory, temporary, 0);
    return error;
}

static int addEnvironment(LSBSpawnPlan *plan, const char *entry) {
    if (plan->envc == LSB_MAX_VECTOR) return E2BIG;
    int error = LSBPlanAddString(plan, entry, &plan->envp[plan->envc]);
    if (!error) ++plan->envc;
    return error;
}

static BOOL controlledEnvironment(const char *entry, NSDictionary<NSString *, NSString *> *environment) {
    const char *equals = strchr(entry, '=');
    if (!equals) return NO;
    NSString *key = [[NSString alloc] initWithBytes:entry length:(NSUInteger)(equals - entry) encoding:NSUTF8StringEncoding];
    if (!key) return NO;
    return environment[key] != nil || [key hasPrefix:@"XPC_SIMULATOR_"] || [key isEqualToString:@"XPC_USERSPACE_REBOOT_TO_BOOT_MODE"] ||
           [key isEqualToString:@"LSB_FAKE_PTY_STATE"] || [key isEqualToString:@"TERM"];
}

int LSBPrepareLaunchdPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]) {
    if (!message || !runtimeRoot || runtimeRoot[0] != '/') return EINVAL;
    @autoreleasepool {
        char canonical[PATH_MAX];
        if (!realpath(runtimeRoot, canonical)) return errno;
        struct stat status;
        if (stat(canonical, &status)) return errno;
        if (!S_ISDIR(status.st_mode)) return ENOTDIR;
        NSString *root = [NSFileManager.defaultManager stringWithFileSystemRepresentation:canonical length:strlen(canonical)];
        NSString *group = root.stringByDeletingLastPathComponent;
        NSString *dataPath = [group stringByAppendingPathComponent:@"SimulatorData"];
        NSString *configPath = [dataPath stringByAppendingPathComponent:@"var/run/LiveSimBooter-launchd-bootstrap.plist"];
        NSString *logPath = [dataPath stringByAppendingPathComponent:@"Library/Logs/LiveSimBooter-launchd.log"];
        if (strlen(canonical) >= LSB_PATH_BYTES || strlen(dataPath.fileSystemRepresentation) >= LSB_PATH_BYTES ||
            strlen(configPath.fileSystemRepresentation) >= LSB_PATH_BYTES) return ENAMETOOLONG;
        NSDictionary<NSString *, NSString *> *runtimeEnvironment = @{
            @"XPC_SIMULATOR_LAUNCHD_NAME": @"com.kdt.LiveSimBooter.launchd",
            @"DYLD_ROOT_PATH": root, @"DYLD_SHARED_CACHE_DIR": [root stringByAppendingPathComponent:@"System/Library/Caches/com.apple.dyld"],
            @"DYLD_FALLBACK_LIBRARY_PATH": [root stringByAppendingPathComponent:@"usr/lib"],
            @"DYLD_FALLBACK_FRAMEWORK_PATH": [root stringByAppendingPathComponent:@"System/Library/Frameworks"],
            @"SIMULATOR_ROOT": root, @"IPHONE_SIMULATOR_ROOT": root, @"SIMULATOR_ARCHS": @"arm64",
            @"SIMULATOR_SHARED_RESOURCES_DIRECTORY": dataPath, @"IPHONE_SHARED_RESOURCES_DIRECTORY": dataPath,
            @"SIMULATOR_LOG_ROOT": [dataPath stringByAppendingPathComponent:@"Library/Logs"],
            @"HOME": dataPath, @"CFFIXED_USER_HOME": dataPath, @"TMPDIR": [dataPath stringByAppendingString:@"/tmp/"],
            @"PATH": [NSString stringWithFormat:@"%@/usr/bin:%@/bin:%@/usr/sbin:%@/sbin", root, root, root, root]
        };
        NSMutableDictionary<NSString *, NSString *> *launchEnvironment = runtimeEnvironment.mutableCopy;
        // The stock launchd wait compares its return value to this selector; an empty child ledger returns -1/ECHILD.
        // This is a version-specific no-child compatibility sentinel, not an actual tank process PID.
        launchEnvironment[@"XPC_SIMULATOR_HOLDING_TANK_HACK"] = @"-1";
        launchEnvironment[@"XPC_FLAGS"] = @"0x0000000000000000";
        launchEnvironment[@"LSB_SIMULATOR_LAUNCHD"] = @"1";
        NSDictionary *configuration = @{
            @"Bootstrap": @{@"Paths": @[], @"Self": launchEnvironment, @"Child": runtimeEnvironment, @"ChildDefaults": @{},
                             @"BinaryOrderPreference": @"arm64", @"SimulatorRuntimeRoot": root},
            @"MultipleInstanceLimit": @64, @"LogToConsole": @YES, @"LogToFlatFile": @NO,
            @"LogServiceLifecycle": @YES, @"LogAllBootstrapErrors": @YES, @"RequireDomainBinding": @NO
        };
        NSData *plist = [NSPropertyListSerialization dataWithPropertyList:configuration format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
        if (!plist) return EINVAL;
        LSBSpawnPlan *plan = calloc(1, sizeof(*plan));
        if (!plan) return ENOMEM;
        plan->version = LSB_PLAN_VERSION;
        plan->creationMask = 022;
        int error = LSBPlanAddString(plan, "/sbin/launchd_sim", &plan->path);
        if (!error) error = LSBPlanAddString(plan, dataPath.fileSystemRepresentation, &plan->cwd);
        if (!error) error = LSBPlanAddString(plan, "/sbin/launchd_sim", &plan->argv[plan->argc++]);
        if (!error) error = LSBPlanAddString(plan, configPath.fileSystemRepresentation, &plan->argv[plan->argc++]);
        for (const char *const *entry = inheritedEnv; !error && entry && *entry; ++entry) {
            if (!controlledEnvironment(*entry, launchEnvironment)) error = addEnvironment(plan, *entry);
        }
        for (NSString *key in [launchEnvironment.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            if (error) break;
            error = addEnvironment(plan, [NSString stringWithFormat:@"%@=%@", key, launchEnvironment[key]].UTF8String);
        }
        if (!error) error = LSBValidateSpawnPlan(plan);
        int directories[7] = {-1, -1, -1, -1, -1, -1, -1}, fds[3] = {-1, -1, -1};
        if (!error && (directories[0] = open(group.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) error = errno;
        const char *names[] = {"SimulatorData", "tmp", "var", "run", "Library", "Logs"};
        const unsigned parents[] = {0, 1, 1, 3, 1, 5};
        for (unsigned i = 0; !error && i < 6; ++i) {
            directories[i + 1] = openDirectory(directories[parents[i]], names[i]);
            if (directories[i + 1] < 0) error = errno;
        }
        if (!error) {
            fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fds[0] < 0) error = errno;
        }
        if (!error) {
            fds[1] = openat(directories[6], "LiveSimBooter-launchd.log", O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
            if (fds[1] < 0) error = errno;
            else if (fstat(fds[1], &status)) error = errno;
            else if (!S_ISREG(status.st_mode) || status.st_nlink != 1) error = EINVAL;
            else if (fcntl(fds[1], F_SETFL, O_WRONLY | O_APPEND)) error = errno;
        }
        if (!error && (fds[2] = fcntl(fds[1], F_DUPFD_CLOEXEC, 3)) < 0) error = errno;
        if (!error) error = writeConfiguration(directories[4], plist);
        if (!error) {
            plan->fdCount = 3;
            for (int i = 0; i < 3; ++i) { plan->fds[i] = (LSBInheritedFD){i, 0}; message->fds[i] = fds[i]; fds[i] = -1; }
            message->plan = *plan;
            fprintf(stderr, "[broker] launchd config=%s log=%s\n", configPath.fileSystemRepresentation, logPath.fileSystemRepresentation);
            // Stock launchd still uses /private/var/tmp/<name> for its own state; this helper does not virtualize that path.
        }
        for (unsigned i = 0; i < 7; ++i) if (directories[i] >= 0) close(directories[i]);
        for (unsigned i = 0; i < 3; ++i) if (fds[i] >= 0) close(fds[i]);
        free(plan);
        return error;
    }
}
