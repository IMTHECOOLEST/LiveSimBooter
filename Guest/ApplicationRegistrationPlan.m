#import "ApplicationRegistrationPlan.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/loader.h>
#include <sys/stat.h>
#include <unistd.h>

static const NSUInteger MaximumApplications = 512;
static const off_t MaximumMetadataBytes = 1024 * 1024;

static id fail(NSError **error, int code, NSString *description) {
    if (error) *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey: description}];
    return nil;
}

static BOOL validString(id value) {
    return [value isKindOfClass:NSString.class] && [value length] && [value canBeConvertedToEncoding:NSUTF8StringEncoding] &&
        [value rangeOfString:@"\0"].location == NSNotFound && [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] < PATH_MAX;
}

static BOOL validGuestPath(id value) {
    if (!validString(value) || ![value hasPrefix:@"/"] || [value hasSuffix:@"/"]) return NO;
    NSArray *parts = [value componentsSeparatedByString:@"/"];
    for (NSUInteger i = 1; i < parts.count; ++i) {
        if (![parts[i] length] || [parts[i] isEqual:@"."] || [parts[i] isEqual:@".."]) return NO;
    }
    return parts.count > 1;
}

static BOOL validAppPath(id path) {
    if (!validGuestPath(path) || ![path hasSuffix:@".app"] || [[path lastPathComponent] length] <= 4) return NO;
    return [@[@"/Applications", @"/System/Applications", @"/System/Library/CoreServices"] containsObject:[path stringByDeletingLastPathComponent]];
}

static NSString *canonicalPath(NSString *path, NSError **error) {
    if (!validString(path) || ![path hasPrefix:@"/"]) return fail(error, EINVAL, @"path must be absolute UTF-8");
    char resolved[PATH_MAX];
    if (!realpath(path.fileSystemRepresentation, resolved)) return fail(error, errno, @"cannot resolve path");
    return [NSFileManager.defaultManager stringWithFileSystemRepresentation:resolved length:strlen(resolved)];
}

static NSString *containedPath(NSString *root, NSString *guestPath, NSError **error) {
    if (!validGuestPath(guestPath)) return fail(error, EINVAL, @"invalid absolute guest path");
    NSString *canonicalRoot = canonicalPath(root, error);
    if (!canonicalRoot) return nil;
    if ([canonicalRoot isEqual:@"/"]) return fail(error, EINVAL, @"RuntimeRoot cannot be the native filesystem root");
    struct stat metadata;
    if (stat(canonicalRoot.fileSystemRepresentation, &metadata) || !S_ISDIR(metadata.st_mode))
        return fail(error, ENOTDIR, @"RuntimeRoot is not a directory");
    NSString *physical = [canonicalRoot stringByAppendingString:guestPath];
    NSString *resolved = canonicalPath(physical, error);
    if (!resolved) return nil;
    // Only the root itself may use a harmless /var alias. Internal symlinks cannot change the selected app or manifest.
    if (![resolved isEqual:physical]) return fail(error, ELOOP, @"guest path traverses a symbolic link");
    return resolved;
}

static NSData *readMetadata(NSString *path, NSError **error) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return fail(error, errno, @"cannot open metadata");
    struct stat metadata;
    int code = fstat(fd, &metadata) ? errno : 0;
    if (!code && !S_ISREG(metadata.st_mode)) code = EINVAL;
    if (!code && (metadata.st_size <= 0 || metadata.st_size > MaximumMetadataBytes)) code = EFBIG;
    NSMutableData *data = code ? nil : [NSMutableData dataWithLength:(NSUInteger)metadata.st_size];
    size_t position = 0;
    while (!code && position < data.length) {
        ssize_t count = pread(fd, (char *)data.mutableBytes + position, data.length - position, (off_t)position);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) code = count < 0 ? errno : EIO;
        else position += (size_t)count;
    }
    close(fd);
    if (code) return fail(error, code, @"metadata must be a readable regular file of at most 1 MiB");
    return data;
}

NSArray<NSString *> *LSBReadApplicationRegistrationManifest(NSString *root, NSString *manifestPath, NSError **error) {
    if (error) *error = nil;
    NSString *physical = containedPath(root, manifestPath, error);
    if (!physical) return nil;
    NSData *data = readMetadata(physical, error);
    if (!data) return nil;
    id manifest = [NSJSONSerialization JSONObjectWithData:data options:0 error:error];
    if (![manifest isKindOfClass:NSDictionary.class] || [manifest count] != 2 || ![manifest[@"version"] isEqual:@1] ||
        ![manifest[@"applications"] isKindOfClass:NSArray.class]) return fail(error, EINVAL, @"invalid application manifest schema");
    NSArray *applications = manifest[@"applications"];
    if (!applications.count || applications.count > MaximumApplications) return fail(error, E2BIG, @"manifest must select 1 through 512 apps");
    NSMutableSet *seen = [NSMutableSet set];
    for (id path in applications) {
        if (!validAppPath(path) || [seen containsObject:path]) return fail(error, EINVAL, @"invalid or duplicate top-level app path");
        [seen addObject:path];
    }
    return [applications copy];
}

NSURL *LSBPrepareApplicationRegistration(NSString *root, NSString *guestPath, NSString **identifier, NSString **skipReason, NSError **error) {
    if (identifier) *identifier = nil;
    if (skipReason) *skipReason = nil;
    if (error) *error = nil;
    if (!validAppPath(guestPath)) return fail(error, EINVAL, @"app must be in an allowed top-level stock directory");
    NSString *physical = containedPath(root, guestPath, error);
    if (!physical) return nil;
    struct stat metadata;
    if (stat(physical.fileSystemRepresentation, &metadata) || !S_ISDIR(metadata.st_mode)) return fail(error, ENOTDIR, @"app is not a directory");
    NSString *infoPath = [physical stringByAppendingPathComponent:@"Info.plist"];
    if (lstat(infoPath.fileSystemRepresentation, &metadata)) {
        if (errno != ENOENT) return fail(error, errno, @"cannot inspect Info.plist");
        if (skipReason) *skipReason = @"missing-info-plist";
        return nil;
    }
    NSData *data = readMetadata(infoPath, error);
    if (!data) return nil;
    id info = [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable format:NULL error:error];
    if (![info isKindOfClass:NSDictionary.class]) return fail(error, EINVAL, @"invalid Info.plist dictionary");
    NSString *bundleID = info[@"CFBundleIdentifier"], *executable = info[@"CFBundleExecutable"];
    if (!validString(bundleID)) return fail(error, EINVAL, @"missing or invalid CFBundleIdentifier");
    if (!executable) {
        if (skipReason) *skipReason = @"no-executable-declaration";
        return nil;
    }
    if (!validString(executable) || [executable containsString:@"/"] || [@[@".", @".."] containsObject:executable])
        return fail(error, EINVAL, @"CFBundleExecutable must be a single filename");
    NSString *executablePath = [physical stringByAppendingPathComponent:executable];
    int fd = open(executablePath.fileSystemRepresentation, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno != ENOENT) return fail(error, errno, @"cannot open app executable");
        if (skipReason) *skipReason = @"missing-executable";
        return nil;
    }
    struct mach_header_64 header;
    BOOL regular = !fstat(fd, &metadata) && S_ISREG(metadata.st_mode);
    ssize_t count;
    do { count = pread(fd, &header, sizeof(header), 0); } while (count < 0 && errno == EINTR);
    close(fd);
    if (!regular || count != sizeof(header) || header.magic != MH_MAGIC_64 || header.cputype != CPU_TYPE_ARM64 || header.filetype != MH_EXECUTE)
        return fail(error, ENOEXEC, @"app executable is not a thin arm64 Mach-O executable");
    if (identifier) *identifier = bundleID;
    return [NSURL fileURLWithPath:physical isDirectory:YES];
}

BOOL LSBVerifyApplicationRegistration(NSURL *expectedURL, NSString *expectedIdentifier, NSURL *registeredURL,
                                    NSString *registeredIdentifier, BOOL installed, NSError **error) {
    if (error) *error = nil;
    if (!installed || ![registeredIdentifier isEqual:expectedIdentifier] || !registeredURL.isFileURL || !expectedURL.isFileURL) {
        fail(error, EIO, @"LaunchServices record is missing or has a different identifier");
        return NO;
    }
    NSString *expected = canonicalPath(expectedURL.path, error);
    if (!expected) return NO;
    NSString *registered = canonicalPath(registeredURL.path, error);
    if (!registered) return NO;
    if (![registered isEqual:expected]) {
        fail(error, EXDEV, @"LaunchServices record points to another bundle");
        return NO;
    }
    return YES;
}
