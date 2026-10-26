#include "../ProcessSupport/LaunchdEndpoint.h"
#include <assert.h>
#include <errno.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#pragma pack(push, 4)
typedef struct { mach_msg_header_t header; mach_msg_body_t body; mach_msg_port_descriptor_t endpoint; NDR_record_t ndr; char name[128]; } Request;
typedef struct { mach_msg_header_t header; NDR_record_t ndr; char name[128]; } RemoveRequest;
#pragma pack(pop)
typedef struct { mach_port_t service, endpoint; unsigned variant; bool remove; } Server;
enum { Success, Denied, WrongGeneration, WrongID, WrongNDR, WrongSize, DelayedReply, ExpiredReply, EncodedExists, EncodedMissing, EncodedDenied };

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
    assert(request->header.msgh_size == (server->remove ? 160u : 176u) && request->header.msgh_id == (server->remove ? 709 : 701));
    assert(request->header.msgh_bits == ((server->remove ? 0 : MACH_MSGH_BITS_COMPLEX) |
                                        MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE, MACH_MSG_TYPE_PORT_SEND)));
    if (server->remove) {
        RemoveRequest *removal = (RemoveRequest *)request;
        assert(!strcmp(removal->name, "IndigoHIDRegistrationPort") && !memcmp(&removal->ndr, &NDR_record, sizeof(NDR_record)));
    } else {
        assert(request->body.msgh_descriptor_count == 1 && request->endpoint.type == MACH_MSG_PORT_DESCRIPTOR);
        assert(request->endpoint.disposition == MACH_MSG_TYPE_PORT_SEND && request->endpoint.name == server->endpoint);
        assert(!strcmp(request->name, "IndigoHIDRegistrationPort") && !memcmp(&request->ndr, &NDR_record, sizeof(NDR_record)));
    }
    if (server->variant == DelayedReply) usleep(1200000); // Longer than the ordinary one-second transaction.
    if (server->variant == ExpiredReply) usleep(100000);
    struct { mig_reply_error_t reply; uint32_t extra; } response = {
        .reply = {.Head = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0), .msgh_size = 36,
                          .msgh_remote_port = request->header.msgh_remote_port, .msgh_id = server->remove ? 809 : 801},
                  .NDR = NDR_record, .RetCode = server->variant == Denied ? 1100 : 0}
    };
    if (server->variant >= EncodedExists)
        response.reply.RetCode = 0xc000 | (server->variant == EncodedExists ? EEXIST : server->variant == EncodedMissing ? ESRCH : EPERM);
    if (server->variant == WrongID) ++response.reply.Head.msgh_id;
    if (server->variant == WrongNDR) ++response.reply.NDR.mig_vers;
    if (server->variant == WrongSize) response.reply.Head.msgh_size = sizeof(response);
    request->header.msgh_remote_port = MACH_PORT_NULL;
    mach_msg_destroy(&request->header);
    mach_msg_return_t sent = mach_msg(&response.reply.Head, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                                     response.reply.Head.msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL);
    if (server->variant == ExpiredReply) {
        // Darwin can consume a send-once reply after its receiver disappeared; either result must leave no retained rights.
        assert(sent == KERN_SUCCESS || sent == MACH_SEND_INVALID_DEST);
        if (sent) mach_msg_destroy(&response.reply.Head);
    } else assert(!sent);
    return NULL;
}

int main(void) {
    alarm(15);
    for (unsigned remove = 0; remove < 2; ++remove) for (unsigned variant = Success; variant <= WrongSize; ++variant) {
        Server server = {.service = newPort(), .endpoint = newPort(), .variant = variant, .remove = remove};
        audit_token_t token = ownAudit();
        if (variant == WrongGeneration) ++token.val[7];
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, serve, &server));
        kern_return_t status = 42;
        errno = EDOM;
        int result = remove ? LSBLaunchdUnregisterEndpointWithTimeout(server.service, &token, "IndigoHIDRegistrationPort", 1000, &status) :
            LSBLaunchdRegisterEndpoint(server.service, &token, "IndigoHIDRegistrationPort", server.endpoint, &status);
        assert(result == (variant == Success ? 0 : variant <= WrongGeneration ? EACCES : EPROTO));
        assert(status == (variant == Success ? 0 : variant == Denied ? 1100 : -1) && errno == EDOM);
        assert(!pthread_join(thread, NULL));
        mach_port_urefs_t refs;
        assert(!mach_port_get_refs(mach_task_self(), server.endpoint, MACH_PORT_RIGHT_SEND, &refs) && refs == 1);
        assert(!mach_port_get_refs(mach_task_self(), server.service, MACH_PORT_RIGHT_SEND, &refs) && refs == 1);
        closePort(server.service); closePort(server.endpoint);
    }
    for (unsigned remove = 0; remove < 2; ++remove) for (unsigned variant = DelayedReply; variant <= ExpiredReply; ++variant) {
        Server server = {.service = newPort(), .endpoint = newPort(), .variant = variant, .remove = remove};
        audit_token_t token = ownAudit();
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, serve, &server));
        kern_return_t status = 42;
        errno = EDOM;
        uint32_t timeout = variant == DelayedReply ? 2000 : 20;
        int result = remove ? LSBLaunchdUnregisterEndpointWithTimeout(server.service, &token, "IndigoHIDRegistrationPort", timeout, &status) :
            LSBLaunchdRegisterEndpointWithTimeout(server.service, &token, "IndigoHIDRegistrationPort", server.endpoint, timeout, &status);
        assert(result == (variant == DelayedReply ? 0 : ETIMEDOUT));
        assert(status == (variant == DelayedReply ? 0 : -1) && errno == EDOM);
        assert(!pthread_join(thread, NULL));
        mach_port_status_t portStatus;
        mach_msg_type_number_t count = MACH_PORT_RECEIVE_STATUS_COUNT;
        assert(!mach_port_get_attributes(mach_task_self(), server.service, MACH_PORT_RECEIVE_STATUS,
                                         (mach_port_info_t)&portStatus, &count) && !portStatus.mps_msgcount);
        mach_port_urefs_t refs;
        assert(!mach_port_get_refs(mach_task_self(), server.endpoint, MACH_PORT_RIGHT_SEND, &refs) && refs == 1);
        assert(!mach_port_get_refs(mach_task_self(), server.service, MACH_PORT_RIGHT_SEND, &refs) && refs == 1);
        closePort(server.service); closePort(server.endpoint);
    }
    for (unsigned remove = 0; remove < 2; ++remove) for (unsigned variant = EncodedExists; variant <= EncodedDenied; ++variant) {
        Server server = {.service = newPort(), .endpoint = newPort(), .variant = variant, .remove = remove};
        audit_token_t token = ownAudit();
        pthread_t thread;
        assert(!pthread_create(&thread, NULL, serve, &server));
        kern_return_t status = -1;
        errno = EDOM;
        int result = remove ? LSBLaunchdUnregisterEndpointWithTimeout(server.service, &token, "IndigoHIDRegistrationPort", 1000, &status) :
            LSBLaunchdRegisterEndpoint(server.service, &token, "IndigoHIDRegistrationPort", server.endpoint, &status);
        int expected = variant == EncodedExists ? EEXIST : variant == EncodedMissing ? ESRCH : EPERM;
        assert(result == expected && status == (0xc000 | expected) && errno == EDOM);
        assert(!pthread_join(thread, NULL));
        closePort(server.service); closePort(server.endpoint);
    }
    mach_port_t service = newPort(), endpoint = newPort();
    audit_token_t token = ownAudit();
    assert(LSBLaunchdRegisterEndpointWithTimeout(service, &token, "IndigoHIDRegistrationPort", endpoint, 0, NULL) == EINVAL);
    assert(LSBLaunchdRegisterEndpointWithTimeout(service, &token, "IndigoHIDRegistrationPort", endpoint, 30001, NULL) == EINVAL);
    assert(LSBLaunchdUnregisterEndpointWithTimeout(service, &token, "IndigoHIDRegistrationPort", 0, NULL) == EINVAL);
    assert(LSBLaunchdUnregisterEndpointWithTimeout(service, &token, "IndigoHIDRegistrationPort", 30001, NULL) == EINVAL);
    assert(LSBLaunchdUnregisterEndpointWithTimeout(service, &token, "", 1000, NULL) == EINVAL);
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
