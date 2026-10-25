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

        // Invalid/oversized plans never publish partial strings or descriptors and do not replace the existing configuration.
        NSData *priorConfig = [NSData dataWithContentsOfFile:configPath];
        const char *tooMany[LSB_MAX_VECTOR + 2];
        for (unsigned i = 0; i < LSB_MAX_VECTOR + 1; ++i) tooMany[i] = "KEEP=1";
        tooMany[LSB_MAX_VECTOR + 1] = NULL;
        assert(LSBPrepareLaunchdPlan(message, root.fileSystemRepresentation, tooMany) == E2BIG);
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
        puts("LaunchdConfiguration: isolated empty bootstrap, preserved debug env, regular stdio, regeneration and failure cleanup passed");
    }
    return 0;
}
