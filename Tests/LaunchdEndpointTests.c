#include "../ProcessSupport/LaunchdEndpoint.h"
#include <assert.h>
#include <errno.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#pragma pack(push, 4)
typedef struct { mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t endpoint; NDR_record_t ndr; char name[128]; } Request;
#pragma pack(pop)
typedef struct { mach_port_t service, endpoint; unsigned variant; } Server;
enum { Success, Denied, WrongGeneration, WrongID, WrongNDR, WrongSize };

static mach_port_t newPort(void) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
    return port;
}
static void closePort(mach_port_t port) {
    assert(!mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_deallocate(mach_task_self(), port));
}
static audit_token_t ownAudit(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    return token;
}
static void *serve(void *context) {
    Server *server = context;
    struct { Request request; uint8_t trailer[MAX_TRAILER_SIZE]; } incoming = {0};
    Request *request = &incoming.request;
    assert(!mach_msg(&request->header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(incoming), server->service, 2000, MACH_PORT_NULL));
    assert(request->header.msgh_size == 176 && request->header.msgh_id == 701);
    assert(request->header.msgh_bits == (MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, MACH_MSG_TYPE_PORT_SEND)));
    assert(request->body.msgh_descriptor_count == 1 && request->endpoint.type == MACH_MSG_PORT_DESCRIPTOR);
    assert(request->endpoint.disposition == MACH_MSG_TYPE_PORT_SEND && request->endpoint.name == server->endpoint);
    assert(!strcmp(request->name, "IndigoHIDRegistrationPort") && !memcmp(&request->ndr, &NDR_record, sizeof(NDR_record)));
    struct { mig_reply_error_t reply; uint32_t extra; } response = {
        .reply = {.Head = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0), .msgh_size = 36,
                          .msgh_remote_port = request->header.msgh_remote_port, .msgh_id = 801},
                  .NDR = NDR_record, .RetCode = server->variant == Denied ? 1100 : 0}
    };
    if (server->variant == WrongID) ++response.reply.Head.msgh_id;
    if (server->variant == WrongNDR) ++response.reply.NDR.mig_vers;
    if (server->variant == WrongSize) response.reply.Head.msgh_size = sizeof(response);
    request->header.msgh_remote_port = MACH_PORT_NULL;
    mach_msg_destroy(&request->header);
    assert(!mach_msg(&response.reply.Head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, response.reply.Head.msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL));
    return NULL;
}

int main(void) {
    alarm(15);
    for (unsigned variant = Success; variant <= WrongSize; ++variant) {
        Server server = {.service = newPort(), .endpoint = newPort(), .variant = variant};
        audit_token_t token = ownAudit();
        if (variant == WrongGeneration) ++token.val[7];
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, serve, &server));
        kern_return_t status = 42;
        errno = EDOM;
        int result = LSBLaunchdRegisterEndpoint(server.service, &token, "IndigoHIDRegistrationPort", server.endpoint, &status);
        assert(result == (variant == Success ? 0 : variant <= WrongGeneration ? EACCES : EPROTO));
        assert(status == (variant == Success ? 0 : variant == Denied ? 1100 : -1) && errno == EDOM);
        assert(!pthread_join(thread, NULL));
        mach_port_urefs_t refs;
        assert(!mach_port_get_refs(mach_task_self(), server.endpoint, MACH_PORT_RIGHT_SEND, &refs) && refs == 1);
        assert(!mach_port_get_refs(mach_task_self(), server.service, MACH_PORT_RIGHT_SEND, &refs) && refs == 1);
        closePort(server.service); closePort(server.endpoint);
    }
    mach_port_t service = newPort(), endpoint = newPort();
    audit_token_t token = ownAudit();
    assert(LSBLaunchdRegisterEndpoint(service, &token, "", endpoint, NULL) == EINVAL);
    char longName[129]; memset(longName, 'x', 128); longName[128] = 0;
    assert(LSBLaunchdRegisterEndpoint(service, &token, longName, endpoint, NULL) == ENAMETOOLONG);
    assert(LSBLaunchdRegisterEndpoint(service, &token, "IndigoHIDRegistrationPort", endpoint, NULL) == ETIMEDOUT);
    closePort(service); // Discard the timed-out request and its copied endpoint right.
    mach_port_urefs_t refs;
    assert(!mach_port_get_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, &refs) && refs == 1);
    closePort(endpoint);
    service = newPort(); endpoint = newPort();
    mach_port_limits_t limits = {.mpl_qlimit = 1};
    assert(!mach_port_set_attributes(mach_task_self(), service, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT));
    mach_msg_header_t filler = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(filler), .msgh_remote_port = service};
    assert(!mach_msg(&filler, MACH_SEND_MSG, sizeof(filler), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL));
    assert(LSBLaunchdRegisterEndpoint(service, &token, "IndigoHIDRegistrationPort", endpoint, NULL) == ETIMEDOUT);
    assert(!mach_port_get_refs(mach_task_self(), endpoint, MACH_PORT_RIGHT_SEND, &refs) && refs == 1);
    assert(!mach_port_get_refs(mach_task_self(), service, MACH_PORT_RIGHT_SEND, &refs) && refs == 1);
    closePort(service); closePort(endpoint);
    puts("LaunchdEndpointTests passed");
    return 0;
}
