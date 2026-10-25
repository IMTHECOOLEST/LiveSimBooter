#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static mach_port_t destroyReplyBeforeSend;
static bool failNextServiceSend;
static mach_msg_return_t testMachMsg(mach_msg_header_t *message, mach_msg_option_t options, mach_msg_size_t sendSize,
                                    mach_msg_size_t receiveSize, mach_port_name_t receive, mach_msg_timeout_t timeout, mach_port_name_t notify) {
    if ((options & MACH_SEND_MSG) && message->msgh_id == 808 && failNextServiceSend) {
        failNextServiceSend = false;
        return MACH_SEND_INVALID_DEST; // Copyin rejection leaves the caller's rights untouched.
    }
    if ((options & MACH_SEND_MSG) && message->msgh_id == 808 && MACH_PORT_VALID(destroyReplyBeforeSend)) {
        assert(!mach_port_mod_refs(mach_task_self(), destroyReplyBeforeSend, MACH_PORT_RIGHT_RECEIVE, -1));
        destroyReplyBeforeSend = MACH_PORT_NULL;
    }
    return mach_msg(message, options, sendSize, receiveSize, receive, timeout, notify);
}
#define mach_msg testMachMsg
#include "../ProcessSupport/LaunchdHandshake.c"
#undef mach_msg

static audit_token_t ownAudit(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    assert(count == TASK_AUDIT_TOKEN_COUNT && token.val[5] == (unsigned)getpid());
    return token;
}

static bool hasRight(mach_port_t port, mach_port_type_t right) {
    mach_port_type_t type;
    return !mach_port_type(mach_task_self(), port, &type) && (type & right);
}

static mach_port_t queueRequest(mach_port_t tank, unsigned size, int id, unsigned replyDisposition) {
    mach_port_t reply;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply));
    struct { mach_msg_header_t header; uint32_t extra; } request = {
        .header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, replyDisposition), .msgh_size = size,
                   .msgh_remote_port = tank, .msgh_local_port = reply, .msgh_id = id}
    };
    assert(!mach_msg(&request.header, MACH_SEND_MSG, size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL));
    return reply;
}

static LSBServiceReply receiveReply(mach_port_t replyPort) {
    struct { LSBServiceReply reply; mach_msg_trailer_t trailer; } response = {0};
    assert(!mach_msg(&response.reply.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(response), replyPort, 5000, MACH_PORT_NULL));
    if (response.reply.header.msgh_id != 808 || response.reply.header.msgh_size != 40)
        fprintf(stderr, "reply id %d size %u\n", response.reply.header.msgh_id, response.reply.header.msgh_size);
    assert(response.reply.header.msgh_id == 808 && response.reply.header.msgh_size == 40);
    assert(response.reply.header.msgh_bits & MACH_MSGH_BITS_COMPLEX);
    assert(response.reply.header.msgh_remote_port == MACH_PORT_NULL && response.reply.body.msgh_descriptor_count == 1);
    assert(response.reply.service.type == MACH_MSG_PORT_DESCRIPTOR && response.reply.service.disposition == MACH_MSG_TYPE_MOVE_RECEIVE);
    assert(hasRight(response.reply.service.name, MACH_PORT_TYPE_RECEIVE));
    return response.reply;
}

static void testRejectionAndRecovery(void) {
    LSBLaunchdHandshake state;
    audit_token_t token = ownAudit();
    errno = EDOM;
    assert(!LSBLaunchdHandshakeInitialize(&state, &token) && errno == EDOM);
    assert(LSBLaunchdHandshakeHandleNext(&state) == EAGAIN && errno == EDOM);
    mach_port_t service = state.serviceReceive;
    assert(service == LSBLaunchdHandshakeServicePort(&state) && hasRight(service, MACH_PORT_TYPE_RECEIVE) && hasRight(service, MACH_PORT_TYPE_SEND));
    for (unsigned variant = 0; variant < 6; ++variant) {
        unsigned size = variant == 0 ? 28 : 24;
        int id = variant == 1 ? 707 : 708;
        unsigned disposition = variant == 2 ? MACH_MSG_TYPE_MAKE_SEND : MACH_MSG_TYPE_MAKE_SEND_ONCE;
        mach_port_t reply = queueRequest(state.tankSend, size, id, disposition);
        if (variant == 3) ++state.expectedAudit.val[7]; // The same PID with a different PID generation must be rejected.
        if (variant == 4) assert(!mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1));
        if (variant == 5) failNextServiceSend = true;
        errno = EDOM;
        int error = LSBLaunchdHandshakeHandleNext(&state);
        int expected = variant < 3 ? EPROTO : variant == 3 ? EACCES : EPIPE;
        if (error != expected) fprintf(stderr, "variant %u returned %d, expected %d\n", variant, error, expected);
        assert(error == expected);
        assert(errno == EDOM && !state.completed && state.serviceReceive == service && hasRight(service, MACH_PORT_TYPE_RECEIVE));
        if (variant == 3) state.expectedAudit = token;
        if (variant != 4) assert(!mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1));
        assert(LSBLaunchdHandshakeHandleNext(&state) == EAGAIN);
    }
    mach_port_t reply = queueRequest(state.tankSend, 24, 708, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    assert(!LSBLaunchdHandshakeHandleNext(&state) && state.completed && !state.serviceReceive && state.tankReceive && state.tankSend);
    assert(LSBLaunchdHandshakeHandleNext(&state) == EALREADY);
    LSBLaunchdHandshakeCloseTank(&state);
    LSBLaunchdHandshakeCloseTank(&state);
    assert(!state.tankReceive && !state.tankSend);
    LSBServiceReply received = receiveReply(reply);
    assert(received.service.name == service); // Existing host send right coalesces with the returned receive right in this test task.
    LSBLaunchdHandshakeDispose(&state);
    assert(hasRight(service, MACH_PORT_TYPE_RECEIVE) && !hasRight(service, MACH_PORT_TYPE_SEND));
    assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1));
    LSBLaunchdHandshakeDispose(&state);
}

static void testMalformedDescriptor(void) {
    LSBLaunchdHandshake state;
    audit_token_t token = ownAudit();
    assert(!LSBLaunchdHandshakeInitialize(&state, &token));
    mach_port_t fixture, reply;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &fixture));
    assert(!mach_port_insert_right(mach_task_self(), fixture, fixture, MACH_MSG_TYPE_MAKE_SEND));
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply));
    LSBServiceReply request = {
        .header = {.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE),
                   .msgh_size = sizeof(request), .msgh_remote_port = state.tankSend, .msgh_local_port = reply, .msgh_id = 708},
        .body = {.msgh_descriptor_count = 1},
        .service = {.name = fixture, .disposition = MACH_MSG_TYPE_MOVE_SEND, .type = MACH_MSG_PORT_DESCRIPTOR}
    };
    assert(!mach_msg(&request.header, MACH_SEND_MSG, sizeof(request), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL));
    assert(LSBLaunchdHandshakeHandleNext(&state) == EPROTO);
    assert(hasRight(fixture, MACH_PORT_TYPE_RECEIVE) && !hasRight(fixture, MACH_PORT_TYPE_SEND));
    assert(!mach_port_mod_refs(mach_task_self(), fixture, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1));
    LSBLaunchdHandshakeDispose(&state);
}

static void testReplyDiesDuringSend(void) {
    LSBLaunchdHandshake state;
    audit_token_t token = ownAudit();
    assert(!LSBLaunchdHandshakeInitialize(&state, &token));
    destroyReplyBeforeSend = queueRequest(state.tankSend, 24, 708, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    int result = LSBLaunchdHandshakeHandleNext(&state);
    // A send-once destination can die after receive: the kernel may consume the reply and its receive right successfully.
    assert(!result && state.completed && !state.serviceReceive && hasRight(state.serviceSend, MACH_PORT_TYPE_DEAD_NAME));
    LSBLaunchdHandshakeCloseTank(&state);
    LSBLaunchdHandshakeDispose(&state);
    LSBLaunchdHandshakeDispose(&state);
}

static void testOversizedRequest(void) {
    LSBLaunchdHandshake state;
    audit_token_t token = ownAudit();
    assert(!LSBLaunchdHandshakeInitialize(&state, &token));
    size_t size = LSB_TANK_MAX_REQUEST + MAX_TRAILER_SIZE + 128;
    mach_msg_header_t *request = calloc(1, size);
    assert(request);
    request->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    request->msgh_size = (mach_msg_size_t)size;
    request->msgh_remote_port = state.tankSend;
    request->msgh_id = 708;
    assert(!mach_msg(request, MACH_SEND_MSG, request->msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL));
    free(request);
    assert(LSBLaunchdHandshakeHandleNext(&state) == EMSGSIZE);
    assert(LSBLaunchdHandshakeHandleNext(&state) == EAGAIN && hasRight(state.serviceReceive, MACH_PORT_TYPE_RECEIVE));
    LSBLaunchdHandshakeDispose(&state);
}

static void writeAll(int fd, const void *data, size_t bytes) {
    const char *cursor = data;
    while (bytes) { ssize_t size = write(fd, cursor, bytes); assert(size > 0); cursor += size; bytes -= (size_t)size; }
}

static void readAll(int fd, void *data, size_t bytes) {
    char *cursor = data;
    while (bytes) { ssize_t size = read(fd, cursor, bytes); assert(size > 0); cursor += size; bytes -= (size_t)size; }
}

static void testChildRoundtrip(void) {
    LSBLaunchdHandshake state;
    audit_token_t token = ownAudit();
    assert(!LSBLaunchdHandshakeInitialize(&state, &token));
    mach_port_t previousBootstrap;
    assert(!task_get_bootstrap_port(mach_task_self(), &previousBootstrap));
    assert(!task_set_bootstrap_port(mach_task_self(), state.tankSend));
    int channel[2];
    assert(!pipe(channel));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        alarm(10);
        close(channel[0]);
        mach_port_t tank;
        assert(!task_get_bootstrap_port(mach_task_self(), &tank) && MACH_PORT_VALID(tank));
        audit_token_t identity = ownAudit();
        writeAll(channel[1], &identity, sizeof(identity));
        mach_port_t reply = queueRequest(tank, 24, 708, MACH_MSG_TYPE_MAKE_SEND_ONCE);
        LSBServiceReply response = receiveReply(reply);
        mach_port_t service = response.service.name;
        assert(!mach_port_insert_right(mach_task_self(), service, service, MACH_MSG_TYPE_MAKE_SEND)); // Stock launchd's next operation.
        struct { mach_msg_header_t header; mach_msg_trailer_t trailer; } ping = {0};
        assert(!mach_msg(&ping.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(ping), service, 5000, MACH_PORT_NULL));
        assert(ping.header.msgh_id == 909);
        char done = 'x';
        writeAll(channel[1], &done, 1);
        assert(!mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1));
        assert(!mach_port_deallocate(mach_task_self(), service));
        _exit(0);
    }
    close(channel[1]);
    assert(!task_set_bootstrap_port(mach_task_self(), previousBootstrap));
    if (MACH_PORT_VALID(previousBootstrap)) assert(!mach_port_deallocate(mach_task_self(), previousBootstrap));
    readAll(channel[0], &state.expectedAudit, sizeof(state.expectedAudit));
    assert(state.expectedAudit.val[5] == (unsigned)child);
    int result = EAGAIN;
    for (unsigned attempt = 0; attempt < 5000 && result == EAGAIN; ++attempt) {
        result = LSBLaunchdHandshakeHandleNext(&state);
        if (result == EAGAIN) usleep(1000);
    }
    if (result) fprintf(stderr, "child request returned %d expected audit pid %u generation %u\n", result,
                        state.expectedAudit.val[5], state.expectedAudit.val[7]);
    assert(!result && state.completed && !state.serviceReceive && hasRight(state.serviceSend, MACH_PORT_TYPE_SEND));
    LSBLaunchdHandshakeCloseTank(&state);
    mach_msg_header_t ping = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), .msgh_size = sizeof(ping),
                              .msgh_remote_port = LSBLaunchdHandshakeServicePort(&state), .msgh_id = 909};
    assert(!mach_msg(&ping, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(ping), 0, MACH_PORT_NULL, 5000, MACH_PORT_NULL));
    char done;
    readAll(channel[0], &done, 1);
    assert(done == 'x');
    close(channel[0]);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(hasRight(state.serviceSend, MACH_PORT_TYPE_DEAD_NAME));
    LSBLaunchdHandshakeDispose(&state);
    LSBLaunchdHandshakeDispose(&state);
}

int main(void) {
    alarm(20);
    LSBLaunchdHandshake invalid;
    memset(&invalid, 0xa5, sizeof(invalid));
    errno = EDOM;
    assert(LSBLaunchdHandshakeInitialize(&invalid, NULL) == EINVAL && errno == EDOM);
    assert(!invalid.tankReceive && !invalid.tankSend && !invalid.serviceReceive && !invalid.serviceSend);
    LSBLaunchdHandshakeDispose(&invalid);
    assert(LSBLaunchdHandshakeHandleNext(&invalid) == EINVAL && errno == EDOM);
    testRejectionAndRecovery();
    testMalformedDescriptor();
    testReplyDiesDuringSend();
    testOversizedRequest();
    testChildRoundtrip();
    puts("LaunchdHandshake: exact708 reply, full-audit rejection, malformed/dead replies, queue draining, forked handoff and right ownership passed");
    return 0;
}
