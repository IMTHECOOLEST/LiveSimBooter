#import "DeviceConfiguration.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *const DevicePreference = @"LSBSimulatorDeviceType";

LSBGuestDeviceType *LSBConfiguredGuestDeviceType(void) {
    id identifier = [NSUserDefaults.standardUserDefaults objectForKey:DevicePreference];
    return [identifier isKindOfClass:NSString.class] ? LSBGuestDeviceTypeForIdentifier(identifier) ?: LSBDefaultGuestDeviceType() :
        LSBDefaultGuestDeviceType();
}
void LSBSaveConfiguredGuestDeviceType(LSBGuestDeviceType *type) {
    [NSUserDefaults.standardUserDefaults setObject:type.identifier forKey:DevicePreference];
}

static int openProfileDirectory(NSString *root, NSArray<NSString *> *components, BOOL create) {
    int directory = open(root.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return -1;
    for (NSString *component in components) {
        if (create && mkdirat(directory, component.fileSystemRepresentation, 0755) && errno != EEXIST) {
            int error = errno; close(directory); errno = error; return -1;
        }
        int next = openat(directory, component.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        int error = errno;
        close(directory);
        if (next < 0) { errno = error; return -1; }
        directory = next;
    }
    return directory;
}

int LSBCopyGuestDeviceCapabilities(NSString *root, LSBGuestDeviceType *type, NSData **output) {
    if (!root.isAbsolutePath || !output || !type || type != LSBGuestDeviceTypeForIdentifier(type.identifier)) return EINVAL;
    *output = nil;
    NSArray *components = @[@"Library", @"Developer", @"CoreSimulator", @"DeviceTypes", type.identifier];
    int directory = openProfileDirectory(root, components, NO);
    int file = directory < 0 ? -1 : openat(directory, "capabilities.plist", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    int error = errno;
    if (directory >= 0) close(directory);
    if (file < 0 && error == ENOENT && type == LSBDefaultGuestDeviceType()) {
        directory = openProfileDirectory(root, @[@"Library", @"Developer", @"CoreSimulator", @"DeviceType"], NO);
        file = directory < 0 ? -1 : openat(directory, "capabilities.plist", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        error = errno;
        if (directory >= 0) close(directory);
    }
    if (file < 0) return error;
    struct stat status;
    error = fstat(file, &status) ? errno : !S_ISREG(status.st_mode) || status.st_size <= 0 || status.st_size > 2 * 1024 * 1024 ? EINVAL : 0;
    NSMutableData *data = error ? nil : [NSMutableData dataWithLength:(NSUInteger)status.st_size];
    size_t offset = 0;
    while (!error && offset < data.length) {
        ssize_t count = read(file, (char *)data.mutableBytes + offset, data.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        offset += (size_t)count;
    }
    close(file);
    if (!error) error = LSBValidateGuestDeviceCapabilities(data, type);
    if (!error) *output = data.copy;
    return error;
}

int LSBInstallGuestDeviceCapabilities(NSString *root, NSData *data) {
    if (!root.isAbsolutePath || !data.length || data.length > 2 * 1024 * 1024) return EINVAL;
    int directory = openProfileDirectory(root, @[@"Library", @"Developer", @"CoreSimulator", @"DeviceType"], YES);
    if (directory < 0) return errno;
    NSString *temporary = [@".capabilities-" stringByAppendingString:NSUUID.UUID.UUIDString];
    int file = openat(directory, temporary.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    int error = file < 0 ? errno : 0;
    size_t offset = 0;
    while (!error && offset < data.length) {
        ssize_t count = write(file, (const char *)data.bytes + offset, data.length - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { error = count < 0 ? errno : EIO; break; }
        offset += (size_t)count;
    }
    if (!error && fsync(file)) error = errno;
    if (file >= 0 && close(file) && !error) error = errno;
    if (!error && renameat(directory, temporary.fileSystemRepresentation, directory, "capabilities.plist")) error = errno;
    if (error) unlinkat(directory, temporary.fileSystemRepresentation, 0);
    close(directory);
    return error;
}
