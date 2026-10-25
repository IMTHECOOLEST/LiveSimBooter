#import <Foundation/Foundation.h>
#include "../Host/LaunchdConfiguration.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void initialize(LSBReceivedMessage *message) {
    memset(message, 0, sizeof(*message));
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) message->fds[i] = -1;
}

static void dispose(LSBReceivedMessage *message) {
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) if (message->fds[i] >= 0) close(message->fds[i]);
    initialize(message);
}

static const char *environmentValue(const LSBSpawnPlan *plan, const char *key) {
    const char *result = NULL;
    size_t length = strlen(key);
    for (unsigned i = 0; i < plan->envc; ++i) {
        const char *entry = plan->strings + plan->envp[i];
        if (!strncmp(entry, key, length) && entry[length] == '=') { assert(!result); result = entry + length + 1; }
    }
    return result;
}

static void assertUnchanged(const LSBReceivedMessage *message) {
    LSBSpawnPlan empty = {0};
    assert(!memcmp(&message->plan, &empty, sizeof(empty)));
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message->fds[i] == -1);
}

static NSDictionary *readPlist(NSString *path) {
    return [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:path] options:NSPropertyListImmutable format:NULL error:NULL];
}

static void testDeviceProfile(LSBReceivedMessage *message, NSString *root, NSString *configPath) {
    const char *overrides[] = {"SIMULATOR_PRODUCT_CLASS=D83", "SIMULATOR_MODEL_IDENTIFIER=iPhone16,1", "SIMULATOR_PRODUCT_CLASS=ignored", NULL};
    assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, overrides));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_PRODUCT_CLASS"), "D83"));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_MODEL_IDENTIFIER"), "iPhone16,1"));
    NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
    for (NSString *scope in @[@"Self", @"Child"]) {
        assert([bootstrap[scope][@"SIMULATOR_PRODUCT_CLASS"] isEqualToString:@"D83"]);
        assert([bootstrap[scope][@"SIMULATOR_MODEL_IDENTIFIER"] isEqualToString:@"iPhone16,1"]);
    }
    dispose(message);
    const char *oneOverride[] = {"SIMULATOR_PRODUCT_CLASS=D83", NULL};
    assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, oneOverride));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_PRODUCT_CLASS"), "D83"));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_MODEL_IDENTIFIER"), "iPhone16,2"));
    dispose(message);
    NSData *previous = [NSData dataWithContentsOfFile:configPath];
    const char *invalid[] = {"SIMULATOR_PRODUCT_CLASS=", "SIMULATOR_MODEL_IDENTIFIER=", "SIMULATOR_PRODUCT_CLASS=\xff",
                             "SIMULATOR_MODEL_IDENTIFIER=\xff"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        const char *environment[] = {invalid[i], NULL};
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, environment) == EINVAL);
        assertUnchanged(message);
        assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:previous]);
    }
}

static void testDFMode(LSBReceivedMessage *message, NSString *root, NSString *dataPath, NSString *configPath, const char *const inherited[]) {
    NSString *jobDirectory = [dataPath stringByAppendingPathComponent:@"Library/LiveSimBooter-LaunchDaemons"];
    NSString *jobPath = [jobDirectory stringByAppendingPathComponent:@"com.kdt.LiveSimBooter.df.plist"];
    NSString *dfLog = [dataPath stringByAppendingPathComponent:@"Library/Logs/df.log"];
    assert(![NSFileManager.defaultManager fileExistsAtPath:jobDirectory]);
    assert(!LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, inherited));
    assert(!LSBValidateSpawnPlan(&message->plan));
    assert(!strcmp(message->plan.strings + message->plan.path, "/sbin/launchd_sim"));
    assert(!strcmp(message->plan.strings + message->plan.argv[1], configPath.fileSystemRepresentation));
    assert(!strcmp(environmentValue(&message->plan, "LSB_DEBUG_STOP_BEFORE_HANDOFF"), "1"));
    NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
    assert([bootstrap[@"Paths"] isEqualToArray:@[jobDirectory]]);
    assert(!bootstrap[@"Child"][@"LSB_SIMULATOR_LAUNCHD"]);
    assert([[NSFileManager.defaultManager contentsOfDirectoryAtPath:jobDirectory error:NULL] isEqualToArray:@[@"com.kdt.LiveSimBooter.df.plist"]]);
    NSDictionary *job = readPlist(jobPath);
    NSSet *expectedKeys = [NSSet setWithArray:@[@"Label", @"ProgramArguments", @"WorkingDirectory", @"StandardOutPath", @"StandardErrorPath",
                                              @"RunAtLoad", @"KeepAlive"]];
    assert([[NSSet setWithArray:job.allKeys] isEqualToSet:expectedKeys]);
    assert([job[@"Label"] isEqualToString:@"com.kdt.LiveSimBooter.df"]);
    assert([job[@"ProgramArguments"] isEqualToArray:@[@"/bin/df"]]);
    assert([job[@"WorkingDirectory"] isEqualToString:@"/private/var/mobile/"]);
    assert([job[@"StandardOutPath"] isEqualToString:@"/private/var/mobile/Library/Logs/df.log"] &&
           [job[@"StandardErrorPath"] isEqualToString:job[@"StandardOutPath"]]);
    assert([job[@"RunAtLoad"] boolValue] && ![job[@"KeepAlive"] boolValue]);
    struct stat originalJob, originalConfig, status;
    assert(!stat(jobPath.fileSystemRepresentation, &originalJob) && S_ISREG(originalJob.st_mode));
    assert(originalJob.st_uid == geteuid() && (originalJob.st_mode & 0777) == 0600);
    assert(!stat(configPath.fileSystemRepresentation, &originalConfig));
    assert(!stat(dfLog.fileSystemRepresentation, &status) && S_ISREG(status.st_mode) && status.st_nlink == 1);
    assert(status.st_uid == geteuid() && (status.st_mode & 0777) == 0600);
    dispose(message);

    // Regeneration replaces both plists atomically and preserves the existing df log.
    assert([@"df prior output\n" writeToFile:dfLog atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    assert(!LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL));
    assert(!stat(jobPath.fileSystemRepresentation, &status) && status.st_ino != originalJob.st_ino);
    assert(!stat(configPath.fileSystemRepresentation, &status) && status.st_ino != originalConfig.st_ino);
    assert([readPlist(jobPath) isEqualToDictionary:job]);
    assert([[NSString stringWithContentsOfFile:dfLog encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"df prior output\n"]);
    dispose(message);
    NSData *priorConfig = [NSData dataWithContentsOfFile:configPath], *priorJob = [NSData dataWithContentsOfFile:jobPath];

    // Unknown jobs, stale temporary files and directories are never loaded or deleted by the probe generator.
    NSString *unexpected = [jobDirectory stringByAppendingPathComponent:@"other.plist"];
    assert([@"unrelated" writeToFile:unexpected atomically:NO encoding:NSUTF8StringEncoding error:NULL]);
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == EEXIST);
    assertUnchanged(message);
    assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:priorConfig]);
    assert([[NSData dataWithContentsOfFile:jobPath] isEqualToData:priorJob]);
    assert([[NSString stringWithContentsOfFile:unexpected encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"unrelated"]);
    assert(!unlink(unexpected.fileSystemRepresentation));
    unexpected = [jobDirectory stringByAppendingPathComponent:@".launchd-bootstrap.stale"];
    assert(!mkdir(unexpected.fileSystemRepresentation, 0700));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == EEXIST);
    assertUnchanged(message);
    assert(!rmdir(unexpected.fileSystemRepresentation));

    // The probe log cannot be a symlink, FIFO, or multiply linked file, and a job symlink cannot redirect publication.
    assert(!unlink(dfLog.fileSystemRepresentation) && !symlink(configPath.fileSystemRepresentation, dfLog.fileSystemRepresentation));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
    assertUnchanged(message);
    assert(!unlink(dfLog.fileSystemRepresentation) && !mkfifo(dfLog.fileSystemRepresentation, 0600));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) != 0);
    assertUnchanged(message);
    assert(!unlink(dfLog.fileSystemRepresentation) && !link(configPath.fileSystemRepresentation, dfLog.fileSystemRepresentation));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == EINVAL);
    assertUnchanged(message);
    assert(!unlink(dfLog.fileSystemRepresentation));
    assert(!unlink(jobPath.fileSystemRepresentation) && !symlink(configPath.fileSystemRepresentation, jobPath.fileSystemRepresentation));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
    assertUnchanged(message);
    assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:priorConfig]);
    assert(!unlink(jobPath.fileSystemRepresentation));
    assert(!rmdir(jobDirectory.fileSystemRepresentation) && !symlink(root.fileSystemRepresentation, jobDirectory.fileSystemRepresentation));
    assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL) != 0);
    assertUnchanged(message);
    assert(!unlink(jobDirectory.fileSystemRepresentation));
    assert(!LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, NULL));
    dispose(message);

    // The original entry point always publishes an empty bootstrap even after the opt-in probe has been prepared.
    assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL));
    assert([readPlist(configPath)[@"Bootstrap"][@"Paths"] count] == 0);
    assert([[NSFileManager.defaultManager contentsOfDirectoryAtPath:jobDirectory error:NULL] isEqualToArray:@[@"com.kdt.LiveSimBooter.df.plist"]]);
    dispose(message);
}

static void testAdditionalJobs(LSBReceivedMessage *message, NSString *root, NSString *dataPath, NSString *configPath) {
    NSData *previous = [NSData dataWithContentsOfFile:configPath];
    assert(LSBPrepareLaunchdBackboardPlan(message, root.fileSystemRepresentation, NULL) == ENOENT);
    assertUnchanged(message);
    assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:previous]);
    NSString *stockDirectory = [root stringByAppendingPathComponent:@"System/Library/LaunchDaemons"];
    assert([NSFileManager.defaultManager createDirectoryAtPath:stockDirectory withIntermediateDirectories:YES attributes:nil error:NULL]);
    NSString *stockPath = [stockDirectory stringByAppendingPathComponent:@"com.apple.backboardd.plist"];
    NSDictionary *services = @{@"com.apple.CARenderServer": @{@"ResetAtClose": @YES}, @"com.apple.backboardd.virtualframebuffer": @YES};
    NSDictionary *stock = @{@"MachServices": services, @"KeepAlive": @YES, @"POSIXSpawnType": @"Interactive", @"UserName": @"root"};
    assert([stock writeToFile:stockPath atomically:YES]);
    const char *inherited[] = {
        "SIMULATOR_FRAMEBUFFER_FRAMEWORK=/System/Library/PrivateFrameworks/SimFramebufferClient.framework/SimFramebufferClient",
        "SIMULATOR_FRAMEBUFFER_FRAMEWORK=", "SIMULATOR_FRAMEBUFFER_FRAMEWORK_EXTRA=preserved", NULL
    };
    NSString *uncanonicalRoot = [root stringByAppendingString:@"/."];
    assert(!LSBPrepareLaunchdBackboardPlan(message, uncanonicalRoot.fileSystemRepresentation, inherited));
    NSString *provider = [root stringByAppendingPathComponent:@"Library/PrivateFrameworks/SimFramebuffer.framework/SimFramebuffer"];
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_FRAMEBUFFER_FRAMEWORK"), provider.fileSystemRepresentation));
    assert(!strcmp(environmentValue(&message->plan, "SIMULATOR_FRAMEBUFFER_FRAMEWORK_EXTRA"), "preserved"));
    NSDictionary *bootstrap = readPlist(configPath)[@"Bootstrap"];
    for (NSString *scope in @[@"Self", @"Child"])
        assert([bootstrap[scope][@"SIMULATOR_FRAMEBUFFER_FRAMEWORK"] isEqualToString:provider]);
    NSString *directory = [dataPath stringByAppendingPathComponent:@"Library/LiveSimBooter-BackboardDaemons"];
    NSDictionary *job = readPlist([directory stringByAppendingPathComponent:@"com.apple.backboardd.plist"]);
    assert([readPlist(configPath)[@"Bootstrap"][@"Paths"] isEqualToArray:@[directory]]);
    assert([job[@"Label"] isEqualToString:@"com.apple.backboardd"] && [job[@"MachServices"] isEqualToDictionary:services]);
    assert([job[@"ProgramArguments"] isEqualToArray:@[@"/usr/libexec/backboardd"]]);
    assert(![job[@"KeepAlive"] boolValue] && ![job[@"EnableTransactions"] boolValue]);
    assert(!job[@"POSIXSpawnType"] && !job[@"UserName"]);
    assert([job[@"StandardOutPath"] isEqualToString:@"/private/var/mobile/Library/Logs/backboardd.log"]);
    dispose(message);
    assert(!LSBPrepareLaunchdLifecyclePlan(message, root.fileSystemRepresentation, NULL));
    assert(!environmentValue(&message->plan, "SIMULATOR_FRAMEBUFFER_FRAMEWORK"));
    for (NSString *scope in @[@"Self", @"Child"])
        assert(!readPlist(configPath)[@"Bootstrap"][scope][@"SIMULATOR_FRAMEBUFFER_FRAMEWORK"]);
    directory = [dataPath stringByAppendingPathComponent:@"Library/LiveSimBooter-LifecycleDaemons"];
    job = readPlist([directory stringByAppendingPathComponent:@"com.kdt.LiveSimBooter.lifecycle.plist"]);
    assert([readPlist(configPath)[@"Bootstrap"][@"Paths"] isEqualToArray:@[directory]]);
    assert([job[@"ProgramArguments"] isEqualToArray:@[@"/bin/lifecycle-probe"]]);
    assert(!job[@"MachServices"] && ![job[@"KeepAlive"] boolValue]);
    dispose(message);
    assert([NSFileManager.defaultManager removeItemAtPath:[root stringByAppendingPathComponent:@"System"] error:NULL]);
}

int main(void) {
    @autoreleasepool {
        char temporary[] = "/tmp/LiveSimBooter-launchd-config.XXXXXX", canonical[PATH_MAX];
        assert(mkdtemp(temporary) && realpath(temporary, canonical));
        NSString *group = [NSString stringWithUTF8String:canonical];
        NSString *root = [group stringByAppendingPathComponent:@"RuntimeRoot"];
        NSString *dataPath = [group stringByAppendingPathComponent:@"SimulatorData"];
        NSString *logPath = [dataPath stringByAppendingPathComponent:@"Library/Logs/LiveSimBooter-launchd.log"];
        assert(!mkdir(root.fileSystemRepresentation, 0700));
        const char *inherited[] = {"LSB_DEBUG_STOP_BEFORE_HANDOFF=1", "KEEP=value=with=equals", "HOME=/wrong", "PATH=/wrong",
            "XPC_SIMULATOR_LAUNCHD_NAME=wrong", "XPC_SIMULATOR_HOLDING_TANK_HACK=999", "XPC_SIMULATOR_HOLDING_TANK_FD_HACK=7",
            "XPC_USERSPACE_REBOOT_TO_BOOT_MODE=wrong", "XPC_FLAGS=0x2", "TERM=xterm", "LSB_FAKE_PTY_STATE=/wrong", "LSB_SIMULATOR_LAUNCHD=0", NULL};
        LSBReceivedMessage *message = calloc(1, sizeof(*message));
        assert(message);
        initialize(message);
        assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, inherited));
        LSBSpawnPlan *plan = &message->plan;
        assert(!LSBValidateSpawnPlan(plan) && plan->argc == 2 && plan->fdCount == 3 && plan->creationMask == 022);
        assert(!strcmp(plan->strings + plan->path, "/sbin/launchd_sim"));
        assert(!strcmp(plan->strings + plan->argv[0], "/sbin/launchd_sim"));
        assert(!strcmp(plan->strings + plan->cwd, dataPath.fileSystemRepresentation));
        assert(!strcmp(environmentValue(plan, "LSB_DEBUG_STOP_BEFORE_HANDOFF"), "1"));
        assert(!strcmp(environmentValue(plan, "KEEP"), "value=with=equals"));
        assert(!strcmp(environmentValue(plan, "XPC_SIMULATOR_HOLDING_TANK_HACK"), "-1"));
        assert(!strcmp(environmentValue(plan, "XPC_FLAGS"), "0x0000000000000000"));
        assert(!strcmp(environmentValue(plan, "LSB_SIMULATOR_LAUNCHD"), "1"));
        assert(!strcmp(environmentValue(plan, "SIMULATOR_PRODUCT_CLASS"), "D84"));
        assert(!strcmp(environmentValue(plan, "SIMULATOR_MODEL_IDENTIFIER"), "iPhone16,2"));
        assert(!environmentValue(plan, "SIMULATOR_FRAMEBUFFER_FRAMEWORK"));
        assert(!strcmp(environmentValue(plan, "HOME"), dataPath.fileSystemRepresentation));
        assert(!environmentValue(plan, "TERM") && !environmentValue(plan, "LSB_FAKE_PTY_STATE"));
        assert(!environmentValue(plan, "XPC_USERSPACE_REBOOT_TO_BOOT_MODE") && !environmentValue(plan, "XPC_SIMULATOR_HOLDING_TANK_FD_HACK"));
        NSString *configPath = [NSString stringWithUTF8String:plan->strings + plan->argv[1]];
        NSDictionary *config = [NSPropertyListSerialization propertyListWithData:[NSData dataWithContentsOfFile:configPath]
            options:NSPropertyListImmutable format:NULL error:NULL];
        assert(config && [config[@"Bootstrap"][@"Paths"] count] == 0);
        assert([config[@"Bootstrap"][@"BinaryOrderPreference"] isEqualToString:@"arm64"]);
        assert([config[@"Bootstrap"][@"SimulatorRuntimeRoot"] isEqualToString:root]);
        assert([config[@"Bootstrap"][@"Self"][@"HOME"] isEqualToString:dataPath]);
        assert([config[@"Bootstrap"][@"Child"][@"HOME"] isEqualToString:dataPath]);
        assert([config[@"Bootstrap"][@"Child"][@"SIMULATOR_HID_SYSTEM_MANAGER"]
            isEqualToString:[root stringByAppendingPathComponent:@"Library/Frameworks/SimulatorHID.framework"]]);
        for (NSString *scope in @[@"Self", @"Child"]) {
            assert([config[@"Bootstrap"][scope][@"SIMULATOR_PRODUCT_CLASS"] isEqualToString:@"D84"]);
            assert([config[@"Bootstrap"][scope][@"SIMULATOR_MODEL_IDENTIFIER"] isEqualToString:@"iPhone16,2"]);
        }
        assert(!config[@"Bootstrap"][@"Child"][@"XPC_SIMULATOR_HOLDING_TANK_HACK"]);
        assert(!config[@"Bootstrap"][@"Child"][@"LSB_DEBUG_STOP_BEFORE_HANDOFF"]);
        assert([config[@"Bootstrap"][@"Self"][@"LSB_SIMULATOR_LAUNCHD"] isEqualToString:@"1"]);
        assert(!config[@"Bootstrap"][@"Child"][@"LSB_SIMULATOR_LAUNCHD"]);
        assert([config[@"MultipleInstanceLimit"] intValue] == 64);
        struct stat output, errorOutput;
        assert(!fstat(message->fds[1], &output) && !fstat(message->fds[2], &errorOutput));
        assert(S_ISREG(output.st_mode) && output.st_ino == errorOutput.st_ino && output.st_dev == errorOutput.st_dev);
        for (int i = 0; i < 3; ++i) {
            assert(!isatty(message->fds[i]) && plan->fds[i].number == i && !plan->fds[i].flags);
            assert(fcntl(message->fds[i], F_GETFD) & FD_CLOEXEC);
        }
        char input;
        assert(read(message->fds[0], &input, 1) == 0);
        assert(write(message->fds[1], "stdout\n", 7) == 7 && write(message->fds[2], "stderr\n", 7) == 7);
        dispose(message);
        assert(!LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL));
        assert(write(message->fds[1], "again\n", 6) == 6);
        dispose(message);
        assert([[NSString stringWithContentsOfFile:logPath encoding:NSUTF8StringEncoding error:NULL] isEqualToString:@"stdout\nstderr\nagain\n"]);
        testDeviceProfile(message, root, configPath);
        testDFMode(message, root, dataPath, configPath, inherited);
        testAdditionalJobs(message, root, dataPath, configPath);

        // Invalid/oversized plans never publish partial strings or descriptors and do not replace the existing configuration.
        NSData *priorConfig = [NSData dataWithContentsOfFile:configPath];
        const char *tooMany[LSB_MAX_VECTOR + 2];
        for (unsigned i = 0; i < LSB_MAX_VECTOR + 1; ++i) tooMany[i] = "KEEP=1";
        tooMany[LSB_MAX_VECTOR + 1] = NULL;
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, tooMany) == E2BIG);
        assertUnchanged(message);
        assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:priorConfig]);
        assert(LSBPrepareLaunchdDFPlan(message, root.fileSystemRepresentation, tooMany) == E2BIG);
        assertUnchanged(message);
        assert([[NSData dataWithContentsOfFile:configPath] isEqualToData:priorConfig]);
        assert(LSBPrepareLaunchdPlan(message, "relative", NULL) == EINVAL);
        assertUnchanged(message);

        // Existing symlinks and non-regular logs must never redirect writes or block on a FIFO.
        assert(!unlink(logPath.fileSystemRepresentation));
        assert(!symlink(configPath.fileSystemRepresentation, logPath.fileSystemRepresentation));
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
        assertUnchanged(message);
        assert(!unlink(logPath.fileSystemRepresentation) && !mkfifo(logPath.fileSystemRepresentation, 0600));
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL) != 0);
        assertUnchanged(message);
        assert(!unlink(logPath.fileSystemRepresentation));
        assert(!unlink(configPath.fileSystemRepresentation));
        assert(!symlink("/dev/null", configPath.fileSystemRepresentation));
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL) == ELOOP);
        assertUnchanged(message);
        assert([NSFileManager.defaultManager removeItemAtPath:dataPath error:NULL]);
        assert(!symlink(root.fileSystemRepresentation, dataPath.fileSystemRepresentation));
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, NULL) != 0);
        assertUnchanged(message);
        assert([NSFileManager.defaultManager contentsOfDirectoryAtPath:root error:NULL].count == 0);
        assert([NSFileManager.defaultManager removeItemAtPath:group error:NULL]);
        free(message);
        puts("LaunchdConfiguration: isolated jobs, device profile defaults/overrides, regular logs, atomic regeneration and failure cleanup passed");
    }
    return 0;
}
