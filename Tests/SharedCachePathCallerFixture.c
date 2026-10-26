#include <dlfcn.h>

void *LSBSharedCachePathOpenSibling(void) {
    return dlopen("@loader_path/libSibling.dylib", RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
}
