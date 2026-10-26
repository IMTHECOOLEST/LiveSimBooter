#ifndef DyldSimLoader_h
#define DyldSimLoader_h

#include <mach-o/loader.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const struct mach_header *header;
    size_t mappedSize;
    uintptr_t slide;
    uintptr_t entry;
} LSBMappedImage;

bool LSBMapDyldSim(const char *path, LSBMappedImage *image, char *error, size_t errorSize);
bool LSBMapSimExecutable(const char *path, LSBMappedImage *image, char *error, size_t errorSize);
// With a cache present, reserve images below its declared range and below the host executable shared region.
// The original APIs retain their unconstrained, loose-library mapping behavior.
bool LSBMapDyldSimForRuntime(const char *path, const char *rootPath, LSBMappedImage *image, char *error, size_t errorSize);
bool LSBMapSimExecutableForRuntime(const char *path, const char *rootPath, LSBMappedImage *image, char *error, size_t errorSize);
// Only unload before entering dyld_sim; it retains pointers into both mapped images.
void LSBUnloadImage(LSBMappedImage *image);

// The image, executable, helpers and argument storage must survive the simulator runtime.
// Detaches the host cache, calls dyld_sim's prepare entry, then invokes its LC_MAIN wrapper with the argument vectors.
// Returns only on a pre-entry failure. If the wrapper returns, exits via a raw syscall without host cleanup.
int LSBEnterDyldSim(const LSBMappedImage *image, const struct mach_header *executable, const char *executablePath,
                   const char *dyldPath, const char *rootPath,
                   int argc, const char *const argv[], const char *const envp[], const char *const apple[]);

#ifdef __cplusplus
}
#endif

#endif
