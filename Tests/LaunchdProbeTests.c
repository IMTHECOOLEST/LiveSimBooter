#include "../ProcessSupport/LaunchdProbe.h"

#include <assert.h>
#include <errno.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
_Static_assert(sizeof(LookupRequest) == 188, "lookup wire layout");

typedef struct { mach_port_t service, fixture; unsigned variant; } Server;
enum { Missing, Denied, WrongAuditGeneration, WrongAuditCredentials, WrongID, WrongSize, WrongNDR, UnexpectedPort, SuccessStatus };

static audit_token_t ownAudit(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    assert(count == TASK_AUDIT_TOKEN_COUNT && token.val[5] == (unsigned)getpid());
    return token;
}

static bool hasRight(mach_port_t port, mach_port_type_t right) {
    mach_port_type_t type = 0;
    return !mach_port_type(mach_task_self(), port, &type) && (type & right);
}

static mach_port_t newService(void) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
    return port;
}

static void closeService(mach_port_t port) {
    assert(!mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_deallocate(mach_task_self(), port));
}

static void *serve(void *context) {
    Server *server = context;
    struct { LookupRequest message; mach_msg_trailer_t trailer; } request = {0};
    assert(!mach_msg(&request.message.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(request), server->service, 2000, MACH_PORT_NULL));
    assert(request.message.header.msgh_size == 188 && request.message.header.msgh_id == 404);
    assert(request.message.header.msgh_bits == MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, MACH_MSG_TYPE_PORT_SEND));
    assert(!memcmp(&request.message.ndr, &NDR_record, sizeof(NDR_record)));
    assert(!strcmp(request.message.name, "com.kdt.LiveSimBooter.health.nonexistent") && !request.message.targetPID && !request.message.flags);
    const uint8_t empty[16] = {0};
    assert(!memcmp(request.message.instance, empty, sizeof(empty)));
    union {
        struct { mig_reply_error_t reply; uint32_t extra; } plain;
        struct { mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t port; } complex;
    } response = {0};
    mach_msg_header_t *header = &response.plain.reply.Head;
    *header = (mach_msg_header_t){.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0),
                                 .msgh_size = 36, .msgh_remote_port = request.message.header.msgh_remote_port, .msgh_id = 504};
    response.plain.reply.NDR = NDR_record;
    response.plain.reply.RetCode = server->variant == Denied ? 1100 : server->variant == SuccessStatus ? 0 : 1102;
    if (server->variant == WrongID) ++header->msgh_id;
    if (server->variant == WrongSize) header->msgh_size = 40;
    if (server->variant == WrongNDR) ++response.plain.reply.NDR.mig_vers;
    if (server->variant == UnexpectedPort) {
        server->fixture = newService();
        header->msgh_size = sizeof(response.complex);
        header->msgh_bits |= MACH_MSGH_BITS_COMPLEX;
        response.complex.body.msgh_descriptor_count = 1;
        response.complex.port = (mach_msg_port_descriptor_t){.name = server->fixture,
            .disposition = MACH_MSG_TYPE_MOVE_SEND, .type = MACH_MSG_PORT_DESCRIPTOR};
    }
    assert(!mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, header->msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL));
    return NULL;
}

static void testAuthenticatedRepliesAndRejection(void) {
    for (unsigned variant = Missing; variant <= SuccessStatus; ++variant) {
        Server server = {.service = newService(), .variant = variant};
        audit_token_t expected = ownAudit();
        if (variant == WrongAuditGeneration) ++expected.val[7];
        if (variant == WrongAuditCredentials) ++expected.val[0];
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, serve, &server));
        kern_return_t status = 42;
        errno = EDOM;
        int result = LSBLaunchdProbeService(server.service, &expected, &status);
        int expectedResult = variant == Missing ? 0 : variant <= WrongAuditCredentials ? EACCES : EPROTO;
        assert(result == expectedResult && errno == EDOM);
        assert(!pthread_join(thread, NULL));
        assert(status == (variant == Missing ? 1102 : variant == Denied ? 1100 : variant == SuccessStatus ? 0 : -1));
        mach_port_urefs_t references = 0;
        assert(!mach_port_get_refs(mach_task_self(), server.service, MACH_PORT_RIGHT_SEND, &references) && references == 1);
        assert(hasRight(server.service, MACH_PORT_TYPE_RECEIVE));
        if (server.fixture) {
            assert(hasRight(server.fixture, MACH_PORT_TYPE_RECEIVE) && !hasRight(server.fixture, MACH_PORT_TYPE_SEND));
            assert(!mach_port_mod_refs(mach_task_self(), server.fixture, MACH_PORT_RIGHT_RECEIVE, -1));
        }
        closeService(server.service);
    }
}

static double monotonicSeconds(void) {
    struct timespec now;
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return now.tv_sec + now.tv_nsec / 1000000000.0;
}

static void testBoundedSendAndReceive(void) {
    for (unsigned full = 0; full < 2; ++full) {
        mach_port_t service = newService();
        if (full) {
            mach_port_limits_t limits = {.mpl_qlimit = 1};
            assert(!mach_port_set_attributes(mach_task_self(), service, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT));
            mach_msg_header_t filler = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0),
                                       .msgh_size = sizeof(filler), .msgh_remote_port = service, .msgh_id = 1};
            assert(!mach_msg(&filler, MACH_SEND_MSG, sizeof(filler), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL));
        }
        audit_token_t expected = ownAudit();
        kern_return_t status = 42;
        double start = monotonicSeconds();
        errno = EDOM;
        assert(LSBLaunchdProbeService(service, &expected, &status) == ETIMEDOUT && errno == EDOM && status == -1);
        double elapsed = monotonicSeconds() - start;
        assert(elapsed >= 0.9 && elapsed < 3.0);
        assert(hasRight(service, MACH_PORT_TYPE_RECEIVE) && hasRight(service, MACH_PORT_TYPE_SEND));
        closeService(service);
    }
}

static void testInvalidAndDeadDestinations(void) {
    audit_token_t expected = ownAudit();
    kern_return_t status = 42;
    errno = EDOM;
    assert(LSBLaunchdProbeService(MACH_PORT_NULL, &expected, &status) == EINVAL && errno == EDOM && status == -1);
    mach_port_t service = newService();
    assert(LSBLaunchdProbeService(service, NULL, NULL) == EINVAL);
    assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(hasRight(service, MACH_PORT_TYPE_DEAD_NAME));
    errno = EDOM;
    assert(LSBLaunchdProbeService(service, &expected, &status) == EPIPE && errno == EDOM && status == -1);
    assert(hasRight(service, MACH_PORT_TYPE_DEAD_NAME));
    assert(!mach_port_deallocate(mach_task_self(), service));
}

int main(void) {
    alarm(10);
    testAuthenticatedRepliesAndRejection();
    testBoundedSendAndReceive();
    testInvalidAndDeadDestinations();
    puts("Launchd probe: exact MIG lookup, full audit identity, malformed replies, right ownership, and bounded timeout passed");
    return 0;
}
