#import "PrivateMetalProbe.h"
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int logSymbol(const char *label, const void *symbol, const char *expectedImage) {
    Dl_info info = {0};
    if (!symbol || !dladdr(symbol, &info) || !info.dli_fname) {
        fprintf(stderr, "[metal-native] symbol=%s address=%p has no image\n", label, symbol);
        return ENOENT;
    }
    fprintf(stderr, "[metal-native] symbol=%s address=%p image=%s base=%p\n", label, symbol, info.dli_fname, info.dli_fbase);
    if (expectedImage) {
        char resolved[PATH_MAX];
        if (!realpath(info.dli_fname, resolved) || strcmp(resolved, expectedImage)) return EXDEV;
    }
    return 0;
}

__attribute__((visibility("default"))) int LSBMetalNativeProbe(const char *absoluteFolder) {
    char folder[PATH_MAX], serializerPath[PATH_MAX], implementationPath[PATH_MAX];
    if (!absoluteFolder || absoluteFolder[0] != '/' || !realpath(absoluteFolder, folder)) return EINVAL;
    if (snprintf(serializerPath, sizeof(serializerPath), "%s/MetalSerializer.dylib", folder) >= (int)sizeof(serializerPath) ||
        snprintf(implementationPath, sizeof(implementationPath), "%s/MTLSimImplementation.dylib", folder) >= (int)sizeof(implementationPath)) return ENAMETOOLONG;
    int status = 0;
    // Objective-C classes remain registered for the lifetime of this isolated probe process; do not unload their images.
    static void *serializerHandle, *implementationHandle;
    fprintf(stderr, "[metal-native] begin folder=%s\n", folder);
    @autoreleasepool {
        @try {
            status = logSymbol("MTLCreateSystemDefaultDevice", (const void *)&MTLCreateSystemDefaultDevice, NULL);
            if (status) return status;
            id<MTLDevice> device = MTLCreateSystemDefaultDevice();
            if (!device) { fprintf(stderr, "[metal-native] native default Metal device is nil\n"); return ENODEV; }
            const char *className = class_getName(object_getClass(device));
            const char *classImage = class_getImageName(object_getClass(device));
            fprintf(stderr, "[metal-native] device class=%s image=%s name=%s registryID=0x%llx\n", className ? className : "(null)",
                classImage ? classImage : "(null)", device.name.UTF8String, (unsigned long long)device.registryID);
            if (!className || strncmp(className, "AGX", 3) || !classImage ||
                strncmp(classImage, "/System/Library/Extensions/AGX", strlen("/System/Library/Extensions/AGX"))) {
                fprintf(stderr, "[metal-native] refusing a simulator device or unverified device wrapper\n");
                return ENOTSUP;
            }
            serializerHandle = dlopen(serializerPath, RTLD_NOW | RTLD_LOCAL);
            if (!serializerHandle) { fprintf(stderr, "[metal-native] serializer dlopen: %s\n", dlerror()); return ENOEXEC; }
            implementationHandle = dlopen(implementationPath, RTLD_NOW | RTLD_LOCAL);
            if (!implementationHandle) { fprintf(stderr, "[metal-native] implementation dlopen: %s\n", dlerror()); return ENOEXEC; }
            LSBCreateMetalDeserializer create = (LSBCreateMetalDeserializer)dlsym(serializerHandle, "MTLCreateDeserializerForDevice");
            const uint32_t *version = dlsym(serializerHandle, "MTLDeserializerVersion");
            void *initialize = dlsym(implementationHandle, "init_with_xpc_connection");
            if ((status = logSymbol("MTLCreateDeserializerForDevice", (const void *)create, serializerPath)) ||
                (status = logSymbol("MTLDeserializerVersion", version, serializerPath)) ||
                (status = logSymbol("init_with_xpc_connection", initialize, implementationPath))) return status;
            fprintf(stderr, "[metal-native] deserializer wire version=%u\n", *version);
            id<LSBMetalNativeDevice> nativeDevice = (id<LSBMetalNativeDevice>)device;
            if (![nativeDevice respondsToSelector:@selector(allowLibrariesFromOtherPlatforms)]) return ENOTSUP;
            [nativeDevice allowLibrariesFromOtherPlatforms];
            fprintf(stderr, "[metal-native] native device accepted allowLibrariesFromOtherPlatforms\n");
            @autoreleasepool {
                id deserializer = create(device, YES);
                if (!deserializer) { fprintf(stderr, "[metal-native] deserializer construction returned nil\n"); return ENOMEM; }
                fprintf(stderr, "[metal-native] constructed deserializer class=%s image=%s\n", class_getName(object_getClass(deserializer)),
                    class_getImageName(object_getClass(deserializer)));
                deserializer = nil;
            }
            fprintf(stderr, "[metal-native] deserializer released; no listener was initialized\n");
        } @catch (NSException *exception) {
            fprintf(stderr, "[metal-native] exception=%s reason=%s\n", exception.name.UTF8String, exception.reason.UTF8String ?: "(none)");
            status = EIO;
        }
    }
    fprintf(stderr, "[metal-native] result=%d\n", status);
    return status;
}
