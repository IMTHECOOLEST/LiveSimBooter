#include "LaunchdHandshake.h"

#include <errno.h>
#include <limits.h>
#include <string.h>

#define LSB_TANK_GET_SERVICE 708
#define LSB_TANK_GET_SERVICE_REPLY 808
#define LSB_TANK_MAX_REQUEST 4096

typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t service;
} LSBServiceReply;

_Static_assert(sizeof(mach_msg_header_t) == 24, "Stock launchd_sim request layout changed");
_Static_assert(sizeof(LSBServiceReply) == 40, "Stock launchd_sim reply layout changed");

static void releaseReceive(mach_port_t *port) {
    if (MACH_PORT_VALID(*port)) mach_port_mod_refs(mach_task_self(), *port, MACH_PORT_RIGHT_RECEIVE, -1);
    *port = MACH_PORT_NULL;
}

static void releaseSend(mach_port_t *port) {
    // Deallocate also releases the dead-name uref if the receiver has already exited.
    if (MACH_PORT_VALID(*port)) mach_port_deallocate(mach_task_self(), *port);
    *port = MACH_PORT_NULL;
}

void LSBLaunchdHandshakeCloseTank(LSBLaunchdHandshake *state) {
    if (!state) return;
    int savedErrno = errno;
    releaseReceive(&state->tankReceive);
    releaseSend(&state->tankSend);
    errno = savedErrno;
}

void LSBLaunchdHandshakeDispose(LSBLaunchdHandshake *state) {
    if (!state) return;
    int savedErrno = errno;
    LSBLaunchdHandshakeCloseTank(state);
    releaseReceive(&state->serviceReceive);
    releaseSend(&state->serviceSend);
    memset(state, 0, sizeof(*state));
    errno = savedErrno;
}

static int allocatePair(mach_port_t *receive, mach_port_t *send) {
    kern_return_t result = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, receive);
    if (result) return result == KERN_RESOURCE_SHORTAGE ? ENOMEM : EIO;
    result = mach_port_insert_right(mach_task_self(), *receive, *receive, MACH_MSG_TYPE_MAKE_SEND);
    if (result) return result == KERN_RESOURCE_SHORTAGE ? ENOMEM : EIO;
    *send = *receive;
    return 0;
}

int LSBLaunchdHandshakeInitialize(LSBLaunchdHandshake *state, const audit_token_t *expectedAudit) {
    if (!state) return EINVAL;
    int savedErrno = errno;
    audit_token_t identity = {{0}};
    if (expectedAudit) identity = *expectedAudit;
    memset(state, 0, sizeof(*state));
    if (!expectedAudit || !identity.val[5] || identity.val[5] > INT_MAX) return EINVAL;
    state->expectedAudit = identity;
    int error = allocatePair(&state->tankReceive, &state->tankSend);
    if (!error) error = allocatePair(&state->serviceReceive, &state->serviceSend);
    if (error) LSBLaunchdHandshakeDispose(state);
    errno = savedErrno;
    return error;
}

mach_port_t LSBLaunchdHandshakeTankPort(const LSBLaunchdHandshake *state) { return state ? state->tankSend : MACH_PORT_NULL; }
mach_port_t LSBLaunchdHandshakeServicePort(const LSBLaunchdHandshake *state) { return state ? state->serviceSend : MACH_PORT_NULL; }

static bool ownsReceive(mach_port_t port) {
    mach_port_type_t type = 0;
    return MACH_PORT_VALID(port) && !mach_port_type(mach_task_self(), port, &type) && (type & MACH_PORT_TYPE_RECEIVE);
}

static int sendService(LSBLaunchdHandshake *state, mach_msg_header_t *request) {
    LSBServiceReply reply = {
        .header = {
            .msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0),
            .msgh_size = sizeof(LSBServiceReply), .msgh_remote_port = request->msgh_remote_port,
            .msgh_id = LSB_TANK_GET_SERVICE_REPLY
        },
        .body = {.msgh_descriptor_count = 1},
        .service = {.name = state->serviceReceive, .disposition = MACH_MSG_TYPE_MOVE_RECEIVE, .type = MACH_MSG_PORT_DESCRIPTOR}
    };
    // The reply now owns the request's send-once right. Request cleanup must not release it a second time.
    request->msgh_remote_port = MACH_PORT_NULL;
    mach_msg_return_t result = mach_msg(&reply.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(reply), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
    if (result == MACH_MSG_SUCCESS) {
        state->serviceReceive = MACH_PORT_NULL;
        state->completed = true;
        return 0;
    }
    // Failed sends can return rights through copyout. Keep a returned receive right rather than destroying the service.
    mach_port_t returnedReceive = reply.service.name;
    if (!ownsReceive(state->serviceReceive))
        state->serviceReceive = ownsReceive(returnedReceive) ? returnedReceive : MACH_PORT_NULL;
    reply.service.name = MACH_PORT_NULL;
    mach_msg_destroy(&reply.header);
    return state->serviceReceive == MACH_PORT_NULL ? EIO : EPIPE;
}

int LSBLaunchdHandshakeHandleNext(LSBLaunchdHandshake *state) {
    if (!state) return EINVAL;
    if (state->completed) return EALREADY;
    if (!MACH_PORT_VALID(state->tankReceive) || !MACH_PORT_VALID(state->serviceReceive)) return EINVAL;
    int savedErrno = errno;
    union {
        mach_msg_header_t header;
        unsigned char bytes[LSB_TANK_MAX_REQUEST + MAX_TRAILER_SIZE];
    } message = {0};
    mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                                MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
    // Deliberately omit MACH_RCV_LARGE: the kernel disposes an oversized request, so it cannot poison the receive queue.
    mach_msg_return_t result = mach_msg(&message.header, options, 0, sizeof(message), state->tankReceive, 0, MACH_PORT_NULL);
    int error = 0;
    if (result == MACH_RCV_TIMED_OUT) error = EAGAIN;
    else if (result == MACH_RCV_TOO_LARGE) error = EMSGSIZE;
    else if (result == MACH_RCV_INTERRUPTED) error = EINTR;
    else if (result) {
        if ((result & ~MACH_MSG_MASK) == MACH_RCV_BODY_ERROR) mach_msg_destroy(&message.header);
        error = EIO;
    }
    else {
        mach_msg_header_t *request = &message.header;
        if (request->msgh_size != sizeof(*request) || request->msgh_id != LSB_TANK_GET_SERVICE ||
            (request->msgh_bits & MACH_MSGH_BITS_COMPLEX) || request->msgh_local_port != state->tankReceive) error = EPROTO;
        else if (!MACH_PORT_VALID(request->msgh_remote_port)) error = EPIPE;
        else if (MACH_MSGH_BITS_REMOTE(request->msgh_bits) != MACH_MSG_TYPE_PORT_SEND_ONCE) error = EPROTO;
        else {
            const mach_msg_audit_trailer_t *trailer = (const void *)(message.bytes + sizeof(*request));
            if (trailer->msgh_trailer_type != MACH_MSG_TRAILER_FORMAT_0 || trailer->msgh_trailer_size < sizeof(*trailer)) error = EPROTO;
            else if (memcmp(&trailer->msgh_audit, &state->expectedAudit, sizeof(state->expectedAudit))) error = EACCES;
            else error = sendService(state, request);
        }
        // This also drops malformed request descriptors and any unused reply/voucher rights.
        mach_msg_destroy(request);
    }
    errno = savedErrno;
    return error;
}
