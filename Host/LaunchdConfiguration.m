#import <Foundation/Foundation.h>
#include "LaunchdConfiguration.h"
#include <dirent.h>
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

static int writeConfiguration(int directory, const char *name, NSData *data) {
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

static int checkJobDirectory(int directory, NSSet<NSString *> *jobNames) {
    int duplicate = fcntl(directory, F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0) return errno;
    DIR *entries = fdopendir(duplicate);
    if (!entries) { int error = errno; close(duplicate); return error; }
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(entries);
        if (!entry) { error = errno; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        NSString *name = [NSString stringWithUTF8String:entry->d_name];
        if (name && [jobNames containsObject:name]) continue;
        // Never bootstrap or remove an unexpected job that was placed beside the generated probe.
        error = EEXIST;
        break;
    }
    closedir(entries);
    return error;
}

static int openRegularLog(int directory, const char *name) {
    int fd = openat(directory, name, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
    if (fd < 0) return -1;
    struct stat status;
    int error = 0;
    if (fstat(fd, &status)) error = errno;
    else if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_uid != geteuid()) error = EINVAL;
    else if (fcntl(fd, F_SETFL, O_WRONLY | O_APPEND)) error = errno;
    if (error) { close(fd); errno = error; return -1; }
    return fd;
}

static int prepareTouchPreferences(int directory, NSData **output) {
    *output = nil;
    NSMutableDictionary *preferences = [NSMutableDictionary dictionary];
    int fd = openat(directory, "com.apple.backboardd.plist", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno != ENOENT) return errno;
    if (fd >= 0) {
        struct stat status;
        int error = 0;
        if (fstat(fd, &status)) error = errno;
        else if (!S_ISREG(status.st_mode) || status.st_nlink != 1 || status.st_uid != geteuid()) error = EINVAL;
        else if (status.st_size < 0 || status.st_size > 1024 * 1024) error = EFBIG;
        NSMutableData *data = !error ? [NSMutableData dataWithLength:(NSUInteger)status.st_size] : nil;
        size_t offset = 0;
        while (!error && offset < data.length) {
            ssize_t count = read(fd, (unsigned char *)data.mutableBytes + offset, data.length - offset);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) { error = count < 0 ? errno : EIO; break; }
            offset += (size_t)count;
        }
        if (!error) {
            unsigned char extra;
            ssize_t count;
            do count = read(fd, &extra, 1); while (count < 0 && errno == EINTR);
            if (count) error = count < 0 ? errno : EAGAIN;
        }
        if (close(fd) && !error) error = errno;
        if (error) return error;
        id existing = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:NULL];
        if (![existing isKindOfClass:NSDictionary.class]) return EINVAL;
        preferences = [existing mutableCopy];
        if ([preferences[@"BKDigitizerVisualizeTouches"] isEqual:@YES]) return 0;
    }
    // BKSLocalDefaults uses this domain/key. Only this isolated simulator's file is changed; never use host NSUserDefaults.
    preferences[@"BKDigitizerVisualizeTouches"] = @YES;
    *output = [NSPropertyListSerialization dataWithPropertyList:preferences format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
    return *output ? 0 : EINVAL;
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

static int prepareLaunchdPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[], NSString *probe) {
    if (!message || !runtimeRoot || runtimeRoot[0] != '/') return EINVAL;
    @autoreleasepool {
        BOOL includeJob = probe != nil, backboard = [probe isEqualToString:@"backboardd"], lifecycle = [probe isEqualToString:@"lifecycle"];
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
        NSString *directoryName = backboard ? @"LiveSimBooter-BackboardDaemons" : lifecycle ? @"LiveSimBooter-LifecycleDaemons" : @"LiveSimBooter-LaunchDaemons";
        NSString *jobDirectory = [[dataPath stringByAppendingPathComponent:@"Library"] stringByAppendingPathComponent:directoryName];
        NSString *label = backboard ? @"com.apple.backboardd" : [@"com.kdt.LiveSimBooter." stringByAppendingString:probe ?: @"df"];
        NSString *jobNameString = [label stringByAppendingString:@".plist"];
        NSString *jobPath = [jobDirectory stringByAppendingPathComponent:jobNameString];
        NSString *logName = [probe ?: @"df" stringByAppendingString:@".log"];
        NSString *jobLogPath = [[dataPath stringByAppendingPathComponent:@"Library/Logs"] stringByAppendingPathComponent:logName];
        if (strlen(canonical) >= LSB_PATH_BYTES || strlen(dataPath.fileSystemRepresentation) >= LSB_PATH_BYTES ||
            strlen(configPath.fileSystemRepresentation) >= LSB_PATH_BYTES ||
            (includeJob && (strlen(jobPath.fileSystemRepresentation) >= LSB_PATH_BYTES ||
                            strlen(jobLogPath.fileSystemRepresentation) >= LSB_PATH_BYTES))) return ENAMETOOLONG;
        NSMutableDictionary<NSString *, NSString *> *runtimeEnvironment = [@{
            @"XPC_SIMULATOR_LAUNCHD_NAME": @"com.kdt.LiveSimBooter.launchd",
            @"DYLD_ROOT_PATH": root, @"DYLD_SHARED_CACHE_DIR": [root stringByAppendingPathComponent:@"System/Library/Caches/com.apple.dyld"],
            @"DYLD_FALLBACK_LIBRARY_PATH": [root stringByAppendingPathComponent:@"usr/lib"],
            @"DYLD_FALLBACK_FRAMEWORK_PATH": [root stringByAppendingPathComponent:@"System/Library/Frameworks"],
            @"SIMULATOR_ROOT": root, @"IPHONE_SIMULATOR_ROOT": root, @"SIMULATOR_ARCHS": @"arm64",
            // CoreSimulator 1171.7 formats this boot's Unix timestamp as %lu. SimulatorHID persists it as its generation key.
            @"SIMULATOR_BOOT_TIME": [NSString stringWithFormat:@"%lu", (unsigned long)NSDate.date.timeIntervalSince1970],
            // Initial iPhone 15 Pro Max profile: CoreSimulator's profile.plist maps iPhone16,2 to productClass D84.
            @"SIMULATOR_PRODUCT_CLASS": @"D84", @"SIMULATOR_MODEL_IDENTIFIER": @"iPhone16,2",
            @"SIMULATOR_HID_SYSTEM_MANAGER": [root stringByAppendingPathComponent:@"Library/Frameworks/SimulatorHID.framework"],
            @"LSB_SIMULATE_SESSIONS": @"1",
            @"SIMULATOR_SHARED_RESOURCES_DIRECTORY": dataPath, @"IPHONE_SHARED_RESOURCES_DIRECTORY": dataPath,
            @"SIMULATOR_LOG_ROOT": [dataPath stringByAppendingPathComponent:@"Library/Logs"],
            @"HOME": dataPath, @"CFFIXED_USER_HOME": dataPath, @"TMPDIR": [dataPath stringByAppendingString:@"/tmp/"],
            @"PATH": [NSString stringWithFormat:@"%@/usr/bin:%@/bin:%@/usr/sbin:%@/sbin", root, root, root, root]
        } mutableCopy];
        if (backboard) {
            runtimeEnvironment[@"LSB_SIMULATE_SYSTEM_POWER"] = @"1";
            // The cached client dlopens getenv directly; an absent value resolves its dispatch table back to its own trampolines.
            runtimeEnvironment[@"SIMULATOR_FRAMEBUFFER_FRAMEWORK"] =
                [root stringByAppendingPathComponent:@"Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer"];
            // Match the host framebuffer's iPhone 15 Pro Max display in both launchd and its children.
            [runtimeEnvironment addEntriesFromDictionary:@{
                @"SIMULATOR_MAINSCREEN_WIDTH": @"1290", @"SIMULATOR_MAINSCREEN_HEIGHT": @"2796",
                @"SIMULATOR_MAINSCREEN_SCALE": @"3", @"SIMULATOR_MAINSCREEN_PITCH": @"460"
            }];
        }
        for (NSString *key in @[@"SIMULATOR_PRODUCT_CLASS", @"SIMULATOR_MODEL_IDENTIFIER"]) {
            const char *name = key.UTF8String;
            size_t length = strlen(name);
            for (const char *const *entry = inheritedEnv; entry && *entry; ++entry) {
                if (strncmp(*entry, name, length) || (*entry)[length] != '=') continue;
                NSString *value = [NSString stringWithUTF8String:*entry + length + 1];
                if (!value.length) return EINVAL;
                runtimeEnvironment[key] = value;
                break; // Match getenv's first occurrence; controlledEnvironment removes all inherited duplicates.
            }
        }
        const char *stopChildren = getenv("LSB_DEBUG_STOP_CHILDREN");
        if (stopChildren && !strcmp(stopChildren, "1")) runtimeEnvironment[@"LSB_DEBUG_STOP_BEFORE_HANDOFF"] = @"1";
        NSMutableDictionary<NSString *, NSString *> *launchEnvironment = runtimeEnvironment.mutableCopy;
        // The stock launchd wait compares its return value to this selector; an empty child ledger returns -1/ECHILD.
        // This is a version-specific no-child compatibility sentinel, not an actual tank process PID.
        launchEnvironment[@"XPC_SIMULATOR_HOLDING_TANK_HACK"] = @"-1";
        launchEnvironment[@"XPC_FLAGS"] = @"0x0000000000000000";
        launchEnvironment[@"LSB_SIMULATOR_LAUNCHD"] = @"1";
        NSDictionary *configuration = @{
            @"Bootstrap": @{@"Paths": includeJob ? @[jobDirectory] : @[], @"Self": launchEnvironment, @"Child": runtimeEnvironment, @"ChildDefaults": @{},
                             @"BinaryOrderPreference": @"arm64", @"SimulatorRuntimeRoot": root},
            @"MultipleInstanceLimit": @64, @"LogToConsole": @YES, @"LogToFlatFile": @NO,
            @"LogServiceLifecycle": @YES, @"LogAllBootstrapErrors": @YES, @"RequireDomainBinding": @NO
        };
        NSData *plist = [NSPropertyListSerialization dataWithPropertyList:configuration format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
        if (!plist) return EINVAL;
        NSMutableDictionary<NSString *, NSData *> *jobPlists = [NSMutableDictionary dictionary];
        if (includeJob) {
            // The trailing slash selects launchd's mobile-directory prefix rule before proxy calls chdir on the translated path.
            NSString *guestLog = [@"/private/var/mobile/Library/Logs" stringByAppendingPathComponent:logName];
            NSString *program = backboard ? @"/usr/libexec/backboardd" : lifecycle ? @"/bin/lifecycle-probe" : @"/bin/df";
            NSMutableDictionary *job = [@{@"Label": label, @"ProgramArguments": @[program],
                                  @"WorkingDirectory": @"/private/var/mobile/", @"StandardOutPath": guestLog, @"StandardErrorPath": guestLog,
                                  @"RunAtLoad": @YES, @"KeepAlive": @NO} mutableCopy];
            if (backboard) {
                // Import only service declarations. Native scheduler/identity policies in the stock plist are not emulated.
                NSString *stockPath = [root stringByAppendingPathComponent:@"System/Library/LaunchDaemons/com.apple.backboardd.plist"];
                NSData *stockData = [NSData dataWithContentsOfFile:stockPath];
                if (!stockData) return ENOENT;
                NSDictionary *stock = [NSPropertyListSerialization propertyListWithData:stockData options:NSPropertyListImmutable format:NULL error:NULL];
                if (![stock isKindOfClass:NSDictionary.class] || ![stock[@"MachServices"] isKindOfClass:NSDictionary.class] ||
                    ![stock[@"MachServices"] count]) return EINVAL;
                job[@"MachServices"] = stock[@"MachServices"];
                job[@"EnableTransactions"] = @NO;
                // Register the physical runtime compiler bundle in backboardd's PID domain; descendants also face an executable-path gate.
                job[@"EnvironmentVariables"] = @{@"LSB_REGISTER_METAL_COMPILER": @"1", @"LSB_SIMULATE_FRAMEWORK_BUNDLES": @"1"};
            }
            NSData *jobPlist = [NSPropertyListSerialization dataWithPropertyList:job format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
            if (!jobPlist) return EINVAL;
            jobPlists[jobNameString] = jobPlist;
            if (backboard) {
                NSString *preferencesLabel = @"com.apple.cfprefsd.xpc.daemon";
                NSString *preferencesName = [preferencesLabel stringByAppendingString:@".plist"];
                NSString *stockPath = [[root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"]
                    stringByAppendingPathComponent:preferencesName];
                NSData *stockData = [NSData dataWithContentsOfFile:stockPath];
                if (!stockData) return ENOENT;
                NSDictionary *stock = [NSPropertyListSerialization propertyListWithData:stockData options:NSPropertyListImmutable format:NULL error:NULL];
                NSArray *arguments = @[@"/usr/sbin/cfprefsd", @"daemon"];
                NSDictionary *services = @{@"com.apple.cfprefsd.daemon": @YES};
                if (![stock isKindOfClass:NSDictionary.class] || ![stock[@"Label"] isEqual:preferencesLabel] ||
                    ![stock[@"ProgramArguments"] isEqual:arguments] || ![stock[@"MachServices"] isEqual:services]) return EINVAL;
                // Simulator CoreFoundation's daemon entry selects this listener, even though the runtime also ships a System-session plist.
                // Keep real job/service names and argv. Native scheduler, resource limits and system-session policy are not emulated.
                NSDictionary *preferencesJob = @{
                    @"Label": stock[@"Label"], @"ProgramArguments": stock[@"ProgramArguments"], @"MachServices": stock[@"MachServices"],
                    @"WorkingDirectory": @"/private/var/mobile/", @"StandardOutPath": @"/private/var/mobile/Library/Logs/cfprefsd.log",
                    @"StandardErrorPath": @"/private/var/mobile/Library/Logs/cfprefsd.log", @"RunAtLoad": @YES,
                    @"KeepAlive": @NO, @"EnableTransactions": @NO
                };
                NSData *preferencesPlist = [NSPropertyListSerialization dataWithPropertyList:preferencesJob
                    format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
                if (!preferencesPlist) return EINVAL;
                jobPlists[preferencesName] = preferencesPlist;
            }
        }
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
        int directories[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1}, fds[3] = {-1, -1, -1};
        NSData *touchPreferences = nil;
        if (!error && (directories[0] = open(group.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC)) < 0) error = errno;
        const char *names[] = {"SimulatorData", "tmp", "var", "run", "Library", "Logs"};
        const unsigned parents[] = {0, 1, 1, 3, 1, 5};
        for (unsigned i = 0; !error && i < 6; ++i) {
            directories[i + 1] = openDirectory(directories[parents[i]], names[i]);
            if (directories[i + 1] < 0) error = errno;
        }
        if (!error && backboard) {
            directories[8] = openDirectory(directories[5], "Preferences");
            if (directories[8] < 0) error = errno;
        }
        if (!error && includeJob) {
            directories[7] = openDirectory(directories[5], directoryName.UTF8String);
            if (directories[7] < 0) error = errno;
            else error = checkJobDirectory(directories[7], [NSSet setWithArray:jobPlists.allKeys]);
        }
        if (!error) {
            fds[0] = open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (fds[0] < 0) error = errno;
        }
        if (!error) {
            fds[1] = openRegularLog(directories[6], "LiveSimBooter-launchd.log");
            if (fds[1] < 0) error = errno;
        }
        if (!error && (fds[2] = fcntl(fds[1], F_DUPFD_CLOEXEC, 3)) < 0) error = errno;
        if (!error && includeJob) {
            int jobLog = openRegularLog(directories[6], logName.UTF8String);
            if (jobLog < 0) error = errno;
            else close(jobLog);
        }
        if (!error && backboard) {
            int preferencesLog = openRegularLog(directories[6], "cfprefsd.log");
            if (preferencesLog < 0) error = errno;
            else close(preferencesLog);
        }
        if (!error && backboard) error = prepareTouchPreferences(directories[8], &touchPreferences);
        for (NSString *name in [jobPlists.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
            if (error) break;
            error = writeConfiguration(directories[7], name.UTF8String, jobPlists[name]);
        }
        if (!error && touchPreferences) error = writeConfiguration(directories[8], "com.apple.backboardd.plist", touchPreferences);
        if (!error) error = writeConfiguration(directories[4], "LiveSimBooter-launchd-bootstrap.plist", plist);
        if (!error) {
            plan->fdCount = 3;
            for (int i = 0; i < 3; ++i) { plan->fds[i] = (LSBInheritedFD){i, 0}; message->fds[i] = fds[i]; fds[i] = -1; }
            message->plan = *plan;
            fprintf(stderr, "[broker] launchd config=%s log=%s jobs=%s\n", configPath.fileSystemRepresentation,
                    logPath.fileSystemRepresentation, probe ? probe.UTF8String : "empty");
            // Stock launchd still uses /private/var/tmp/<name> for its own state; this helper does not virtualize that path.
        }
        for (unsigned i = 0; i < 9; ++i) if (directories[i] >= 0) close(directories[i]);
        for (unsigned i = 0; i < 3; ++i) if (fds[i] >= 0) close(fds[i]);
        free(plan);
        return error;
    }
}

int LSBPrepareLaunchdPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]) {
    return prepareLaunchdPlan(message, runtimeRoot, inheritedEnv, nil);
}

int LSBPrepareLaunchdDFPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]) {
    return prepareLaunchdPlan(message, runtimeRoot, inheritedEnv, @"df");
}

int LSBPrepareLaunchdBackboardPlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]) {
    return prepareLaunchdPlan(message, runtimeRoot, inheritedEnv, @"backboardd");
}

int LSBPrepareLaunchdLifecyclePlan(LSBReceivedMessage *message, const char *runtimeRoot, const char *const inheritedEnv[]) {
    return prepareLaunchdPlan(message, runtimeRoot, inheritedEnv, @"lifecycle");
}
