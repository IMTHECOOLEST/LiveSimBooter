#ifndef LSB_NATIVE_METAL_CONTROL_H
#define LSB_NATIVE_METAL_CONTROL_H

#include <mach/message.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LSB_NATIVE_METAL_CONTROL_SIZE 4096u
#define LSB_NATIVE_METAL_CONTROL_VERSION 2u

// Opaque, pointer-free shared ABI. Initialize before sharing; never reinitialize a published mapping.
typedef struct LSBNativeMetalControl LSBNativeMetalControl;
typedef struct {
    uint64_t generation;
    bool authorized;
    bool publicAccess;
    audit_token_t audit;
} LSBNativeMetalAuthorization;

// All functions return errno values. Initialize/Validate require exactly SIZE bytes and natural 8-byte alignment.
int LSBNativeMetalControlInitialize(LSBNativeMetalControl *control, size_t size);
int LSBNativeMetalControlValidate(const LSBNativeMetalControl *control, size_t size);
// NULL revokes. Invalid tokens return EINVAL without changing the record. Concurrent publishers return EBUSY; serialize host updates.
// Successful publication is immediately visible to subsequent coherent reads. Exhaustion permanently fails closed with EOVERFLOW.
int LSBNativeMetalControlPublish(LSBNativeMetalControl *control, const audit_token_t *audit);
// Authorize any client for a session-owned public helper. Publishes a zero audit token; Publish(NULL) revokes this mode too.
int LSBNativeMetalControlPublishPublic(LSBNativeMetalControl *control);
// Requires a complete mapped page. Clears output on every error. A coherent revoked snapshot returns 0 with authorized=false.
// Makes at most eight attempts; an in-progress publication returns EAGAIN. No locks, allocation, or unbounded waits.
int LSBNativeMetalControlRead(const LSBNativeMetalControl *control, LSBNativeMetalAuthorization *authorization);

// Create an unlinked 0600 regular file in directory. Caller owns the RW mapping and separate read-only, close-on-exec FD.
int LSBNativeMetalControlCreate(const char *directory, LSBNativeMetalControl **writer, int *readOnlyFD);
// Borrow the read-only FD, validate regular-file type/size/access mode, and create an independent PROT_READ shared mapping.
int LSBNativeMetalControlImport(int readOnlyFD, const LSBNativeMetalControl **reader);
void LSBNativeMetalControlUnmap(const LSBNativeMetalControl *control);

#ifdef __cplusplus
}
#endif
#endif
