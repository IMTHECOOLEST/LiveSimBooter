#import "../Host/IndigoHIDServer.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t word(const uint8_t *bytes, size_t offset) {
    uint32_t value;
    memcpy(&value, bytes + offset, sizeof(value));
    return value;
}

static audit_token_t ownAudit(void) {
    audit_token_t audit;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &count));
    return audit;
}

static mach_port_t receivePort(void) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    return port;
}

static void destroyPort(mach_port_t port) {
    assert(!mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
}

static void registerPort(LSBIndigoHIDServer *server, mach_port_t events) {
    mach_msg_header_t message = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND),
        .msgh_size = sizeof(message), .msgh_remote_port = server.registrationPort, .msgh_local_port = events};
    assert(!mach_msg(&message, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(message), 0, MACH_PORT_NULL, 100, MACH_PORT_NULL));
}

static void receiveMessage(mach_port_t port, uint8_t bytes[4096]) {
    memset(bytes, 0, 4096);
    mach_msg_header_t *header = (void *)bytes;
    assert(!mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 4096, port, 1000, MACH_PORT_NULL));
    assert(!header->msgh_id && header->msgh_remote_port == MACH_PORT_NULL);
}

static void noMessage(mach_port_t port) {
    uint8_t bytes[4096] = {0};
    assert(mach_msg((void *)bytes, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(bytes), port, 0, MACH_PORT_NULL) == MACH_RCV_TIMED_OUT);
}

static LSBIndigoHIDServer *newServer(void) {
    int error = 42;
    LSBIndigoHIDServer *server = [[LSBIndigoHIDServer alloc] initWithError:&error];
    assert(server && !error);
    return server;
}

static void connectServer(LSBIndigoHIDServer *server, mach_port_t port) {
    audit_token_t audit = ownAudit();
    [server authorizeClientWithAuditToken:&audit];
    registerPort(server, port);
    for (unsigned i = 0; i < 500 && !server.connectedClientPID; ++i) usleep(1000);
    assert(server.connectedClientPID == getpid());
    uint8_t bytes[4096];
    receiveMessage(port, bytes);
    assert(((mach_msg_header_t *)bytes)->msgh_size == 180 && word(bytes, 24) == 148 && bytes[28] == 1);
    assert(word(bytes, 32) == 0x7fff0001); // Existing authenticated touch-device setup still comes first.
}

static void receiveHome(mach_port_t port, BOOL pressed) {
    uint8_t bytes[4096];
    receiveMessage(port, bytes);
    assert(((mach_msg_header_t *)bytes)->msgh_size == 180 && word(bytes, 24) == 148 && bytes[28] == 1);
    assert(!bytes[29] && !bytes[30] && !bytes[31]);
    const uint8_t *payload = bytes + 32;
    uint64_t timestamp;
    memcpy(&timestamp, payload + 4, sizeof(timestamp));
    // Independent stock factory values: IndigoHIDMessageForButton(0, operation, 51), with the receiver's 148-byte stride.
    assert(word(payload, 0) == 2 && timestamp && !word(payload, 12));
    assert(!word(payload, 16) && word(payload, 20) == (pressed ? 1u : 2u) && word(payload, 24) == 51);
    for (size_t i = 28; i < 144; ++i) assert(!payload[i]);
    assert(payload[144] == 255 && !payload[145] && !payload[146] && !payload[147]);
}

static void receiveKeyboard(mach_port_t port, uint32_t usage, BOOL pressed) {
    uint8_t bytes[4096];
    receiveMessage(port, bytes);
    assert(word(bytes, 48) == 10001 && word(bytes, 52) == (pressed ? 1u : 2u));
    assert(word(bytes, 56) == 100 && word(bytes, 60) == usage && word(bytes, 68) == 7);
}

static void setSingleMessageLimit(mach_port_t port) {
    mach_port_limits_t limits = {.mpl_qlimit = 1};
    assert(!mach_port_set_attributes(mach_task_self(), port, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT));
}

static void exactPacketsAndAuthentication(void) {
    LSBIndigoHIDServer *server = newServer();
    mach_port_t events = receivePort();
    assert(![server releaseHomeButton] && [server sendHomeButtonPressed:YES] == ENOTCONN);
    audit_token_t wrong = ownAudit();
    ++wrong.val[7];
    [server authorizeClientWithAuditToken:&wrong];
    registerPort(server, events);
    usleep(30000);
    assert(!server.connectedClientPID && [server sendHomeButtonPressed:YES] == ENOTCONN);
    noMessage(events);
    connectServer(server, events);
    assert([server sendHomeButtonPressed:NO] == ENOENT);
    assert(![server sendHomeButtonPressed:YES]);
    receiveHome(events, YES);
    assert([server sendHomeButtonPressed:YES] == EALREADY);
    noMessage(events);
    assert(![server sendHomeButtonPressed:NO]);
    receiveHome(events, NO);
    assert([server sendHomeButtonPressed:NO] == ENOENT && ![server releaseHomeButton]);
    noMessage(events);

    // Cleanup is Home-specific: a live touch and a held keyboard modifier keep their independent state.
    assert(![server sendKeyboardUsage:0xe1 pressed:YES]);
    receiveKeyboard(events, 0xe1, YES);
    assert(![server sendTouch:7 phase:LSBIndigoTouchBegan x:0.5 y:0.5]);
    uint8_t bytes[4096];
    receiveMessage(events, bytes);
    assert(bytes[28] == 2 && word(bytes, 32) == 11);
    assert(![server sendHomeButtonPressed:YES]);
    receiveHome(events, YES);
    assert(![server releaseHomeButton]);
    receiveHome(events, NO);
    assert(![server releaseAllKeys]);
    receiveKeyboard(events, 0xe1, NO);
    assert(![server sendTouch:7 phase:LSBIndigoTouchEnded x:0.5 y:0.5]);
    receiveMessage(events, bytes);
    assert(bytes[28] == 2 && word(bytes, 32) == 11);
    [server authorizeClientWithAuditToken:NULL];
    assert(![server releaseHomeButton] && [server sendHomeButtonPressed:YES] == ENOTCONN);
    [server close];
    assert(![server releaseHomeButton] && [server sendHomeButtonPressed:YES] == EBADF);
    destroyPort(events);
}

static void releaseBackpressure(void) {
    LSBIndigoHIDServer *server = newServer();
    mach_port_t events = receivePort();
    connectServer(server, events);
    setSingleMessageLimit(events);
    assert(![server sendKeyboardUsage:4 pressed:YES]); // Occupy the queue before the Home down.
    assert([server sendHomeButtonPressed:YES] == EAGAIN);
    assert([server sendHomeButtonPressed:NO] == ENOENT && ![server releaseHomeButton]); // Failed downs never become held.
    receiveKeyboard(events, 4, YES);
    assert(![server releaseAllKeys]);
    receiveKeyboard(events, 4, NO);

    for (unsigned cleanup = 0; cleanup < 2; ++cleanup) {
        assert(![server sendHomeButtonPressed:YES]);
        assert((cleanup ? [server releaseHomeButton] : [server sendHomeButtonPressed:NO]) == EAGAIN);
        assert([server sendHomeButtonPressed:YES] == EBUSY && [server sendHomeButtonPressed:NO] == EBUSY);
        assert([server releaseHomeButton] == EAGAIN);
        receiveHome(events, YES);
        receiveHome(events, NO); // No additional UI calls: automatic retry delivers exactly one up.
        assert([server sendHomeButtonPressed:NO] == ENOENT && ![server releaseHomeButton]);
        noMessage(events);
    }
    assert(![server sendHomeButtonPressed:YES]);
    receiveHome(events, YES);
    assert(![server releaseHomeButton]);
    receiveHome(events, NO);
    [server close];
    destroyPort(events);
}

static void reconnectDropsPendingReleases(void) {
    LSBIndigoHIDServer *server = newServer();
    mach_port_t retired = receivePort();
    connectServer(server, retired);
    setSingleMessageLimit(retired);
    assert(![server sendHomeButtonPressed:YES] && [server releaseHomeButton] == EAGAIN);
    [server authorizeClientWithAuditToken:NULL];
    receiveHome(retired, YES);
    mach_port_t replacement = receivePort();
    connectServer(server, replacement);
    usleep(60000); // A pending release from the retired connection must not reach either event port.
    noMessage(retired);
    noMessage(replacement);
    assert([server sendHomeButtonPressed:NO] == ENOENT);
    assert(![server sendHomeButtonPressed:YES]);
    receiveHome(replacement, YES);
    assert(![server releaseHomeButton]);
    receiveHome(replacement, NO);
    setSingleMessageLimit(replacement);
    assert(![server sendHomeButtonPressed:YES] && [server releaseHomeButton] == EAGAIN);
    [server close];
    receiveHome(replacement, YES);
    usleep(60000);
    noMessage(replacement); // Closing also retires pending retries without manufacturing an up on a new connection.
    assert(![server releaseHomeButton]);
    destroyPort(retired);
    destroyPort(replacement);
}

int main(void) {
    alarm(15);
    @autoreleasepool {
        exactPacketsAndAuthentication();
        releaseBackpressure();
        reconnectDropsPendingReleases();
    }
    puts("IndigoHomeButtonTests: stock packet, authenticated routing, isolated state and generation-bound releases passed");
    return 0;
}
