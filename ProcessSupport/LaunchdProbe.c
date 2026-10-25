#include "LaunchdProbe.h"

#include <errno.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

enum { LookupRequestID = 404, LookupReplyID = 504, UnknownService = 1102, NotPrivileged = 1100 };

// The stock 23A343 MIG server validates this packed layout before entering its read-only service lookup.
// bootstrap_look_up() itself uses libxpc's cached global pipe, so it cannot probe an arbitrary simulator service port.
#pragma pack(push, 4)
typedef struct {
    mach_msg_header_t header;
    NDR_record_t ndr;
    char name[128];
    int32_t targetPID;
    uint8_t instance[16];
    uint64_t flags;
} LookupRequest;
#pragma pack(pop)
_Static_assert(sizeof(LookupRequest) == 188 && offsetof(LookupRequest, flags) == 180, "23A343 MIG lookup request layout");
_Static_assert(sizeof(mig_reply_error_t) == 36, "23A343 MIG lookup error reply layout");

static int64_t monotonicNanoseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static mach_msg_timeout_t remainingMilliseconds(int64_t deadline) {
    int64_t now = monotonicNanoseconds();
    if (now < 0 || now >= deadline) return 0;
    return (mach_msg_timeout_t)((deadline - now + 999999) / 1000000);
}

static int probe(mach_port_t service, const audit_token_t *expectedAudit, kern_return_t *bootstrapStatus) {
    if (bootstrapStatus) *bootstrapStatus = -1;
    if (!MACH_PORT_VALID(service) || !expectedAudit || !expectedAudit->val[5]) return EINVAL;
    int64_t now = monotonicNanoseconds();
    if (now < 0) return errno;
    int64_t deadline = now + 1000000000;
    mach_port_t replyPort = MACH_PORT_NULL;
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &replyPort)) return ENOMEM;
    LookupRequest request = {
        .header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE),
                   .msgh_size = sizeof(request), .msgh_remote_port = service, .msgh_local_port = replyPort, .msgh_id = LookupRequestID},
        .ndr = NDR_record, .name = "com.kdt.LiveSimBooter.health.nonexistent"
    };
    int result = ETIMEDOUT;
    mach_msg_return_t sent;
    do {
        mach_msg_timeout_t timeout = remainingMilliseconds(deadline);
        if (!timeout) goto cleanup;
        sent = mach_msg(&request.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT | MACH_SEND_INTERRUPT,
                        sizeof(request), 0, MACH_PORT_NULL, timeout, MACH_PORT_NULL);
    } while (sent == MACH_SEND_INTERRUPTED);
    if (sent) { result = sent == MACH_SEND_TIMED_OUT ? ETIMEDOUT : sent == MACH_SEND_INVALID_DEST ? EPIPE : EIO; goto cleanup; }
    union { mach_msg_header_t header; uint8_t bytes[4096 + MAX_TRAILER_SIZE]; } response = {0};
    mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_INTERRUPT |
        MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
    mach_msg_return_t received;
    do {
        mach_msg_timeout_t timeout = remainingMilliseconds(deadline);
        if (!timeout) goto cleanup;
        received = mach_msg(&response.header, options, 0, sizeof(response), replyPort, timeout, MACH_PORT_NULL);
    } while (received == MACH_RCV_INTERRUPTED);
    if (received) { result = received == MACH_RCV_TIMED_OUT ? ETIMEDOUT : received == MACH_RCV_TOO_LARGE ? EMSGSIZE : EIO; goto cleanup; }
    result = EPROTO;
    if (response.header.msgh_size == sizeof(mig_reply_error_t) && response.header.msgh_id == LookupReplyID &&
        response.header.msgh_bits == MACH_MSGH_BITS(0, MACH_MSG_TYPE_PORT_SEND_ONCE) &&
        response.header.msgh_local_port == replyPort && response.header.msgh_remote_port == MACH_PORT_NULL &&
        response.header.msgh_voucher_port == MACH_PORT_NULL) {
        mig_reply_error_t reply;
        mach_msg_audit_trailer_t trailer;
        memcpy(&reply, response.bytes, sizeof(reply));
        memcpy(&trailer, response.bytes + sizeof(reply), sizeof(trailer));
        if (trailer.msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0 && trailer.msgh_trailer_size >= sizeof(trailer) &&
            !memcmp(&reply.NDR, &NDR_record, sizeof(reply.NDR))) {
            result = EACCES;
            if (!memcmp(&trailer.msgh_audit, expectedAudit, sizeof(*expectedAudit))) {
                if (bootstrapStatus) *bootstrapStatus = reply.RetCode;
                result = reply.RetCode == UnknownService ? 0 : reply.RetCode == NotPrivileged ? EACCES : EPROTO;
            }
        }
    }
    // Reject and release any unexpected returned rights, without touching our receive right or the caller's borrowed service right.
    mach_msg_destroy(&response.header);
cleanup:
    mach_port_mod_refs(mach_task_self(), replyPort, MACH_PORT_RIGHT_RECEIVE, -1);
    return result;
}

int LSBLaunchdProbeService(mach_port_t service, const audit_token_t *expectedAudit, kern_return_t *bootstrapStatus) {
    int savedErrno = errno;
    int result = probe(service, expectedAudit, bootstrapStatus);
    errno = savedErrno;
    return result;
}
