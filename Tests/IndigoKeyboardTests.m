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

static void connectServer(LSBIndigoHIDServer *server, mach_port_t port) {
    audit_token_t audit = ownAudit();
    [server authorizeClientWithAuditToken:&audit];
    registerPort(server, port);
    for (unsigned i = 0; i < 500 && !server.connectedClientPID; ++i) usleep(1000);
    assert(server.connectedClientPID == getpid());
    uint8_t bytes[4096];
    receiveMessage(port, bytes);
    assert(((mach_msg_header_t *)bytes)->msgh_size == 180 && word(bytes, 24) == 148 && bytes[28] == 1);
    assert(word(bytes, 32) == 0x7fff0001); // The existing touch-device setup still precedes input.
}

static LSBIndigoHIDServer *newServer(void) {
    int error = 42;
    LSBIndigoHIDServer *server = [[LSBIndigoHIDServer alloc] initWithError:&error];
    assert(server && !error);
    return server;
}

static uint32_t receiveKey(mach_port_t port, BOOL pressed) {
    uint8_t bytes[4096];
    receiveMessage(port, bytes);
    assert(((mach_msg_header_t *)bytes)->msgh_size == 180 && word(bytes, 24) == 148 && bytes[28] == 1);
    assert(!bytes[29] && !bytes[30] && !bytes[31]);
    const uint8_t *payload = bytes + 32;
    uint64_t timestamp;
    memcpy(&timestamp, payload + 4, sizeof(timestamp));
    // Independent offsets from SimulatorKit IndigoHIDMessageForHIDArbitrary(100, 7, usage, operation).
    assert(word(payload, 0) == 2 && timestamp && !word(payload, 12));
    assert(word(payload, 16) == 10001 && word(payload, 20) == (pressed ? 1u : 2u));
    assert(word(payload, 24) == 100 && !word(payload, 32) && word(payload, 36) == 7);
    for (size_t i = 40; i < 144; ++i) assert(!payload[i]);
    assert(payload[144] == 255 && !payload[145] && !payload[146] && !payload[147]);
    return word(payload, 28);
}

static void exactPacketsAndAuthentication(void) {
    LSBIndigoHIDServer *server = newServer();
    mach_port_t events = receivePort();
    assert([server sendKeyboardUsage:4 pressed:YES] == ENOTCONN);
    audit_token_t incorrect = ownAudit();
    ++incorrect.val[7];
    [server authorizeClientWithAuditToken:&incorrect];
    registerPort(server, events);
    usleep(30000);
    assert(!server.connectedClientPID && [server sendKeyboardUsage:4 pressed:YES] == ENOTCONN);
    noMessage(events);
    connectServer(server, events);
    const uint32_t invalid[] = {0, 1, 2, 3, 0xe8, UINT32_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert([server sendKeyboardUsage:invalid[i] pressed:YES] == EINVAL);
        assert([server sendKeyboardUsage:invalid[i] pressed:NO] == EINVAL);
    }
    noMessage(events);
    const uint32_t usages[] = {4, 0x28, 0x2a, 0x50, 0x39, 0xe0, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7};
    for (unsigned i = 0; i < sizeof(usages) / sizeof(usages[0]); ++i) {
        uint32_t usage = usages[i];
        assert([server sendKeyboardUsage:usage pressed:NO] == ENOENT);
        assert(![server sendKeyboardUsage:usage pressed:YES] && receiveKey(events, YES) == usage);
        assert([server sendKeyboardUsage:usage pressed:YES] == EALREADY); // Guest repeat logic owns held-key repeats.
        noMessage(events);
        assert(![server sendKeyboardUsage:usage pressed:NO] && receiveKey(events, NO) == usage);
        assert([server sendKeyboardUsage:usage pressed:NO] == ENOENT);
    }
    assert(![server releaseAllKeys]);
    noMessage(events);
    assert(![server sendTouch:7 phase:LSBIndigoTouchBegan x:0.5 y:0.5]);
    uint8_t bytes[4096];
    receiveMessage(events, bytes);
    assert(bytes[28] == 2 && word(bytes, 32) == 11);
    assert(![server sendKeyboardUsage:0xe1 pressed:YES] && receiveKey(events, YES) == 0xe1);
    assert(![server releaseAllKeys] && receiveKey(events, NO) == 0xe1);
    assert(![server sendTouch:7 phase:LSBIndigoTouchEnded x:0.5 y:0.5]); // Key release must not cancel a live contact.
    receiveMessage(events, bytes);
    assert(bytes[28] == 2 && word(bytes, 32) == 11);
    [server authorizeClientWithAuditToken:NULL];
    assert(!server.connectedClientPID && [server sendKeyboardUsage:4 pressed:YES] == ENOTCONN);
    [server close];
    assert([server sendKeyboardUsage:4 pressed:YES] == EBADF);
    destroyPort(events);
}

static void setSingleMessageLimit(mach_port_t port) {
    mach_port_limits_t limits = {.mpl_qlimit = 1};
    assert(!mach_port_set_attributes(mach_task_self(), port, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT));
}

static void releaseBackpressure(void) {
    LSBIndigoHIDServer *server = newServer();
    mach_port_t events = receivePort();
    connectServer(server, events);
    setSingleMessageLimit(events);
    assert(![server sendKeyboardUsage:4 pressed:YES]);
    assert([server sendKeyboardUsage:5 pressed:YES] == EAGAIN);
    assert([server sendKeyboardUsage:5 pressed:NO] == ENOENT); // Failed down never records a pressed key.
    assert(receiveKey(events, YES) == 4);
    assert(![server sendKeyboardUsage:0xe1 pressed:YES]);
    assert([server sendKeyboardUsage:4 pressed:NO] == EAGAIN);
    assert([server sendKeyboardUsage:6 pressed:YES] == EBUSY);
    assert([server releaseAllKeys] == EAGAIN); // Focus loss merges the still-held modifier into an existing key-up retry.
    assert(receiveKey(events, YES) == 0xe1);
    assert(receiveKey(events, NO) == 4); // No more UI calls: the pending release is retried as the queue drains.
    assert(receiveKey(events, NO) == 0xe1);
    assert([server sendKeyboardUsage:4 pressed:NO] == ENOENT);
    assert(![server sendKeyboardUsage:0xe0 pressed:YES] && receiveKey(events, YES) == 0xe0);
    assert(![server sendKeyboardUsage:0xe1 pressed:YES] && receiveKey(events, YES) == 0xe1);
    assert(![server sendKeyboardUsage:4 pressed:YES]);
    assert([server releaseAllKeys] == EAGAIN);
    assert([server sendKeyboardUsage:7 pressed:YES] == EBUSY);
    assert(receiveKey(events, YES) == 4);
    BOOL releasedLetter = NO, releasedControl = NO, releasedShift = NO;
    for (unsigned i = 0; i < 3; ++i) {
        uint32_t usage = receiveKey(events, NO);
        if (usage == 4) { assert(!releasedLetter); releasedLetter = YES; }
        else if (usage == 0xe0) { assert(!releasedControl); releasedControl = YES; }
        else { assert(usage == 0xe1 && !releasedShift); releasedShift = YES; }
    }
    assert(releasedLetter && releasedControl && releasedShift);
    assert(![server releaseAllKeys]);
    noMessage(events);
    assert(![server sendKeyboardUsage:4 pressed:YES] && receiveKey(events, YES) == 4);
    assert(![server sendKeyboardUsage:4 pressed:NO] && receiveKey(events, NO) == 4);
    [server close];
    destroyPort(events);
}

static void reconnectDropsPendingReleases(void) {
    LSBIndigoHIDServer *server = newServer();
    mach_port_t retired = receivePort();
    connectServer(server, retired);
    setSingleMessageLimit(retired);
    assert(![server sendKeyboardUsage:0xe1 pressed:YES]);
    assert([server releaseAllKeys] == EAGAIN);
    [server authorizeClientWithAuditToken:NULL];
    assert(receiveKey(retired, YES) == 0xe1);
    mach_port_t replacement = receivePort();
    connectServer(server, replacement);
    usleep(60000); // A retired generation's scheduled retry cannot send into either connection.
    noMessage(retired);
    noMessage(replacement);
    assert(![server sendKeyboardUsage:0xe1 pressed:YES] && receiveKey(replacement, YES) == 0xe1);
    assert(![server releaseAllKeys] && receiveKey(replacement, NO) == 0xe1);
    [server close];
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
    puts("IndigoKeyboardTests: stock packets, authenticated routing, repeat ownership and release retries passed");
    return 0;
}
