#include "LaunchdEndpoint.h"

#include <errno.h>
#include <limits.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#pragma pack(push, 4)
typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t endpoint;
    NDR_record_t ndr;
    char name[128];
} RegisterRequest;
#pragma pack(pop)
_Static_assert(sizeof(RegisterRequest) == 176 && offsetof(RegisterRequest, name) == 48, "23A343 bridge registration layout");
_Static_assert(sizeof(mig_reply_error_t) == 36, "23A343 bridge registration reply layout");

static int64_t monotonicNanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static mach_msg_timeout_t remainingMilliseconds(int64_t deadline) {
    int64_t now = monotonicNanoseconds();
    return now < 0 || now >= deadline ? 0 : (mach_msg_timeout_t)((deadline - now + 999999) / 1000000);
}

// Reconstructed from iOS 26.0 (23A343) RuntimeRoot/usr/lib/system/host/liblaunch_sim.dylib:
// launch_sim_register_endpoint calls launch_mig_bridge_register (MIG request 701, reply 801).
// Sim26Booter's use of the original API: https://github.com/khanhduytran0/Sim26Booter/blob/main/SimBooter/main.m
// This version accepts an already-resolved service port and adds reply audit validation and a bounded deadline.
static int LSB_launch_sim_register_endpoint(mach_port_t service, const audit_token_t *audit, const char *name, mach_port_t endpoint,
                                           uint32_t timeoutMilliseconds, kern_return_t *status) {
    if (status) *status = -1;
    if (!timeoutMilliseconds || timeoutMilliseconds > 30000) return EINVAL;
    if (!MACH_PORT_VALID(service) || !MACH_PORT_VALID(endpoint) || !audit || !audit->val[5] || audit->val[5] > INT_MAX || !name) return EINVAL;
    size_t length = strnlen(name, 128);
    if (!length) return EINVAL;
    if (length == 128) return ENAMETOOLONG;
    int64_t now = monotonicNanoseconds();
    if (now < 0) return errno;
    int64_t deadline = now + (int64_t)timeoutMilliseconds * 1000000;
    mach_port_t replyPort = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &replyPort)) return ENOMEM;
    RegisterRequest request = {
        .header = {.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE),
                   .msgh_size = sizeof(request), .msgh_remote_port = service, .msgh_local_port = replyPort, .msgh_id = 701},
        .body = {.msgh_descriptor_count = 1},
        .endpoint = {.name = endpoint, .disposition = MACH_MSG_TYPE_COPY_SEND, .type = MACH_MSG_PORT_DESCRIPTOR},
        .ndr = NDR_record
    };
    memcpy(request.name, name, length + 1);
    int result = ETIMEDOUT;
    mach_msg_timeout_t sendTimeout = remainingMilliseconds(deadline);
    if (!sendTimeout) goto cleanup;
    mach_msg_return_t sent = mach_msg(&request.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT | MACH_SEND_INTERRUPT,
                                     sizeof(request), 0, MACH_PORT_NULL, sendTimeout, MACH_PORT_NULL);
    if (sent) {
        // A failed send after copyin returns copied rights with received dispositions, distinct from our borrowed COPY_SEND rights.
        if (MACH_MSGH_BITS_REMOTE(request.header.msgh_bits) == MACH_MSG_TYPE_PORT_SEND) mach_msg_destroy(&request.header);
        result = sent == MACH_SEND_TIMED_OUT ? ETIMEDOUT : sent == MACH_SEND_INVALID_DEST ? EPIPE : sent == MACH_SEND_INTERRUPTED ? EINTR : EIO;
        goto cleanup;
    }
    union { mach_msg_header_t header; uint8_t bytes[4096 + MAX_TRAILER_SIZE]; } response = {0};
    mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_INTERRUPT |
        MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
    mach_msg_return_t received;
    do {
        mach_msg_timeout_t timeout = remainingMilliseconds(deadline);
        if (!timeout) goto cleanup;
        received = mach_msg(&response.header, options, 0, sizeof(response), replyPort, timeout, MACH_PORT_NULL);
    } while (received == MACH_RCV_INTERRUPTED);
    if (received) {
        if ((received & ~MACH_MSG_MASK) == MACH_RCV_BODY_ERROR) mach_msg_destroy(&response.header);
        result = received == MACH_RCV_TIMED_OUT ? ETIMEDOUT : received == MACH_RCV_TOO_LARGE ? EMSGSIZE : EIO;
        goto cleanup;
    }
    result = EPROTO;
    if (response.header.msgh_size == sizeof(mig_reply_error_t) && response.header.msgh_id == 801 &&
        response.header.msgh_bits == MACH_MSGH_BITS(0, MACH_MSG_TYPE_PORT_SEND_ONCE) && response.header.msgh_local_port == replyPort &&
        response.header.msgh_remote_port == MACH_PORT_NULL && response.header.msgh_voucher_port == MACH_PORT_NULL) {
        mig_reply_error_t reply;
        mach_msg_audit_trailer_t trailer;
        memcpy(&reply, response.bytes, sizeof(reply));
        memcpy(&trailer, response.bytes + sizeof(reply), sizeof(trailer));
        if (trailer.msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0 && trailer.msgh_trailer_size >= sizeof(trailer) &&
            !memcmp(&reply.NDR, &NDR_record, sizeof(reply.NDR))) {
            result = EACCES;
            if (!memcmp(&trailer.msgh_audit, audit, sizeof(*audit))) {
                if (status) *status = reply.RetCode;
                result = reply.RetCode == KERN_SUCCESS ? 0 : reply.RetCode == 1100 ? EACCES : reply.RetCode == 1101 ? EEXIST : EIO;
            }
        }
    }
    mach_msg_destroy(&response.header);
cleanup:
    mach_port_mod_refs(mach_task_self(), replyPort, MACH_PORT_RIGHT_RECEIVE, -1);
    return result;
}

int LSBLaunchdRegisterEndpoint(mach_port_t service, const audit_token_t *expectedAudit, const char *name,
                              mach_port_t endpoint, kern_return_t *bootstrapStatus) {
    return LSBLaunchdRegisterEndpointWithTimeout(service, expectedAudit, name, endpoint, 1000, bootstrapStatus);
}

int LSBLaunchdRegisterEndpointWithTimeout(mach_port_t service, const audit_token_t *expectedAudit, const char *name,
                                         mach_port_t endpoint, uint32_t timeoutMilliseconds, kern_return_t *bootstrapStatus) {
    int savedErrno = errno;
    int result = LSB_launch_sim_register_endpoint(service, expectedAudit, name, endpoint, timeoutMilliseconds, bootstrapStatus);
    errno = savedErrno;
    return result;
}
