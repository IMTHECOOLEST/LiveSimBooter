#import "../Host/IndigoHIDServer.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static audit_token_t ownAudit(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    return token;
}

static mach_port_t receivePort(void) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    return port;
}

static void registerClient(mach_port_t registration, mach_port_t events, mach_msg_id_t identifier) {
    mach_msg_header_t header = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND),
        .msgh_size = sizeof(header), .msgh_remote_port = registration, .msgh_local_port = events, .msgh_id = identifier};
    assert(!mach_msg(&header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(header), 0, MACH_PORT_NULL, 100, MACH_PORT_NULL));
}

static void awaitRegistration(LSBIndigoHIDServer *server) {
    for (unsigned i = 0; i < 500 && !server.connectedClientPID; ++i) usleep(1000);
    assert(server.connectedClientPID == getpid());
}

static uint32_t uintAt(const uint8_t *buffer, size_t offset) { uint32_t value; memcpy(&value, buffer + offset, sizeof(value)); return value; }
static double doubleAt(const uint8_t *buffer, size_t offset) { double value; memcpy(&value, buffer + offset, sizeof(value)); return value; }

static void receiveTouches(mach_port_t port, uint8_t *bytes, unsigned count) {
    memset(bytes, 0, 4096);
    mach_msg_header_t *header = (void *)bytes;
    assert(!mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 4096, port, 1000, MACH_PORT_NULL));
    assert(header->msgh_size == 32 + count * 148 && header->msgh_id == 0 && header->msgh_remote_port == MACH_PORT_NULL);
    assert(uintAt(bytes, 24) == 148 && bytes[28] == count && uintAt(bytes, 32) == 11);
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *payload = bytes + 32 + i * 148;
        // Stock 1171.7 IndigoHIDDataTarget selects digitizer targets from byte 76, independently of the finger identity.
        assert(uintAt(payload, 76) == 50 && payload[144] == (i ? 0 : 255) && payload[145] == 0);
    }
}

static void receiveDigitizer(mach_port_t port) {
    uint8_t bytes[4096] = {0};
    mach_msg_header_t *header = (void *)bytes;
    assert(!mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(bytes), port, 1000, MACH_PORT_NULL));
    assert(header->msgh_size == 32 + 148 && header->msgh_id == 0 && header->msgh_remote_port == MACH_PORT_NULL);
    assert(uintAt(bytes, 24) == 148 && bytes[28] == 1);
    // These offsets come from stock SimHIDSystem's control-message decoder, not the host's structures.
    assert(uintAt(bytes, 32) == 0x7fff0001 && uintAt(bytes, 48) == 1 && uintAt(bytes, 64) == 0x40000001);
    assert(bytes[68] == 1 && !strcmp((char *)bytes + 69, "PurpleMain") && bytes[32 + 144] == 255 && !bytes[32 + 145]);
}

static void digitizerBackpressure(void) {
    int error;
    LSBIndigoHIDServer *server = [[LSBIndigoHIDServer alloc] initWithError:&error];
    assert(server && !error);
    mach_port_t events = receivePort();
    mach_port_limits_t limits = {.mpl_qlimit = 1};
    assert(!mach_port_set_attributes(mach_task_self(), events, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT));
    mach_msg_header_t filler = {.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0), .msgh_size = sizeof(filler),
        .msgh_remote_port = events, .msgh_id = 77};
    assert(!mach_msg(&filler, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(filler), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL));
    audit_token_t audit = ownAudit();
    [server authorizeClientWithAuditToken:&audit];
    registerClient(server.registrationPort, events, 0);
    awaitRegistration(server);
    // Registration could not queue setup. Until it succeeds, no contact is recorded or sent with an unknown target.
    assert([server sendTouch:1 phase:LSBIndigoTouchBegan x:0.5 y:0.5] == EAGAIN);
    uint8_t bytes[4096] = {0};
    mach_msg_header_t *header = (void *)bytes;
    assert(!mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(bytes), events, 1000, MACH_PORT_NULL));
    assert(header->msgh_id == 77);
    // This retry queues setup and then encounters the full queue; the following retry must not duplicate setup.
    assert([server sendTouch:1 phase:LSBIndigoTouchBegan x:0.5 y:0.5] == EAGAIN);
    receiveDigitizer(events);
    assert(![server sendTouch:1 phase:LSBIndigoTouchBegan x:0.5 y:0.5]);
    receiveTouches(events, bytes, 2);
    [server close];
    assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
}

static void assertContact(const uint8_t *bytes, unsigned index, uint32_t mask, double x, double y, BOOL down) {
    const uint8_t *payload = bytes + 32 + index * 148;
    assert(uintAt(payload, 24) == mask && doubleAt(payload, 28) == x && doubleAt(payload, 36) == y);
    assert(uintAt(payload, 68) == (unsigned)down && uintAt(payload, 72) == (unsigned)down);
}

static void edgeGestures(void) {
    int error;
    LSBIndigoHIDServer *server = [[LSBIndigoHIDServer alloc] initWithError:&error];
    assert(server && !error);
    mach_port_t events = receivePort();
    audit_token_t audit = ownAudit();
    [server authorizeClientWithAuditToken:&audit];
    registerClient(server.registrationPort, events, 0);
    awaitRegistration(server);
    receiveDigitizer(events);
    uint8_t bytes[4096];
    // Captured from the installed SimulatorKit mouse factory, including the origin bits on its final up.
    const uint32_t masks[] = {0, 0x02040000, 0x08040000, 0x01040000, 0x04040000};
    assert([server sendTouch:1 phase:LSBIndigoTouchBegan x:0.5 y:1 edge:(LSBIndigoTouchEdge)5] == EINVAL);
    assert([server sendTouch:1 phase:LSBIndigoTouchBegan x:0.5 y:1 edge:(LSBIndigoTouchEdge)-1] == EINVAL);
    for (unsigned edge = 0; edge <= 4; ++edge) {
        assert(![server sendTouch:1 phase:LSBIndigoTouchBegan x:0.5 y:1 edge:(LSBIndigoTouchEdge)edge]);
        receiveTouches(events, bytes, 2);
        assertContact(bytes, 0, masks[edge] | 3, 0.5, 1, YES);
        assertContact(bytes, 1, masks[edge] | 3, 0.5, 1, YES);
        uint32_t identity = uintAt(bytes, 32 + 148 + 20);
        assert([server sendTouch:1 phase:LSBIndigoTouchMoved x:0.5 y:0.5 edge:LSBIndigoTouchEdgeTop] == EINVAL);
        assert(![server sendTouch:1 phase:LSBIndigoTouchMoved x:0.5 y:0.5]);
        receiveTouches(events, bytes, 2);
        assertContact(bytes, 0, masks[edge] | 4, 0.5, 0.5, YES);
        assertContact(bytes, 1, masks[edge] | 4, 0.5, 0.5, YES);
        assert(uintAt(bytes, 32 + 148 + 20) == identity);
        assert(![server sendTouch:1 phase:LSBIndigoTouchEnded x:0.5 y:0.5]);
        receiveTouches(events, bytes, 2);
        assertContact(bytes, 0, masks[edge] | 3, 0.5, 0.5, NO);
        assertContact(bytes, 1, masks[edge] | 3, 0.5, 0.5, NO);
    }
    assert(![server sendTouch:10 phase:LSBIndigoTouchBegan x:0.5 y:1 edge:LSBIndigoTouchEdgeBottom]);
    receiveTouches(events, bytes, 2);
    assert(![server sendTouch:20 phase:LSBIndigoTouchBegan x:1 y:0.5 edge:LSBIndigoTouchEdgeRight]);
    receiveTouches(events, bytes, 3);
    assertContact(bytes, 0, 0x04040003, 1, 0.5, YES);
    assertContact(bytes, 1, 0x01040000, 0.5, 1, YES); // An unchanged sibling retains its own origin, without phase changes.
    assertContact(bytes, 2, 0x04040003, 1, 0.5, YES);
    assert(![server sendTouch:10 phase:LSBIndigoTouchEnded x:0.5 y:0.5]);
    receiveTouches(events, bytes, 3);
    assertContact(bytes, 0, 0x01040003, 0.5, 0.5, YES); // The other finger keeps the collection active.
    assertContact(bytes, 1, 0x01040003, 0.5, 0.5, NO);
    assertContact(bytes, 2, 0x04040000, 1, 0.5, YES);
    assert(![server sendTouch:30 phase:LSBIndigoTouchBegan x:0.5 y:0.5]);
    receiveTouches(events, bytes, 3);
    assertContact(bytes, 0, 3, 0.5, 0.5, YES); // Slot reuse does not retain the previous bottom origin.
    assertContact(bytes, 1, 3, 0.5, 0.5, YES);
    assert(![server sendTouch:30 phase:LSBIndigoTouchMoved x:0.5 y:1]);
    receiveTouches(events, bytes, 3);
    assertContact(bytes, 0, 4, 0.5, 1, YES); // Reaching an edge later does not create a system gesture.
    assertContact(bytes, 1, 4, 0.5, 1, YES);
    assert(![server cancelTouches]);
    receiveTouches(events, bytes, 3);
    assertContact(bytes, 0, 0x83, 0.5, 1, NO);
    assertContact(bytes, 1, 0x83, 0.5, 1, NO);
    assertContact(bytes, 2, 0x04040083, 1, 0.5, NO);

    mach_port_limits_t limits = {.mpl_qlimit = 1};
    assert(!mach_port_set_attributes(mach_task_self(), events, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT));
    assert(![server sendTouch:40 phase:LSBIndigoTouchBegan x:0.5 y:1 edge:LSBIndigoTouchEdgeBottom]);
    assert([server sendTouch:50 phase:LSBIndigoTouchBegan x:0.5 y:0 edge:LSBIndigoTouchEdgeTop] == EAGAIN);
    receiveTouches(events, bytes, 2);
    uint32_t identity = uintAt(bytes, 32 + 148 + 20);
    assert(![server sendTouch:50 phase:LSBIndigoTouchBegan x:1 y:0.5 edge:LSBIndigoTouchEdgeRight]);
    assert([server sendTouch:40 phase:LSBIndigoTouchMoved x:0.5 y:0.25] == EAGAIN);
    assert([server cancelTouches] == EAGAIN);
    receiveTouches(events, bytes, 3);
    assertContact(bytes, 0, 0x04040003, 1, 0.5, YES); // Failed top begin never latched its origin.
    receiveTouches(events, bytes, 3); // Cancellation retries preserve the last successfully delivered coordinates and edges.
    assertContact(bytes, 0, 0x01040083, 0.5, 1, NO);
    assertContact(bytes, 1, 0x01040083, 0.5, 1, NO);
    assertContact(bytes, 2, 0x04040083, 1, 0.5, NO);
    assert(uintAt(bytes, 32 + 148 + 20) == identity);
    assert(![server sendTouch:40 phase:LSBIndigoTouchBegan x:0.5 y:0 edge:LSBIndigoTouchEdgeTop]);
    receiveTouches(events, bytes, 2);
    assert(![server sendTouch:40 phase:LSBIndigoTouchCancelled x:0.5 y:0.25]);
    receiveTouches(events, bytes, 2);
    assertContact(bytes, 0, 0x02040083, 0.5, 0.25, NO);
    assertContact(bytes, 1, 0x02040083, 0.5, 0.25, NO);
    [server authorizeClientWithAuditToken:NULL];
    [server authorizeClientWithAuditToken:&audit];
    registerClient(server.registrationPort, events, 0);
    awaitRegistration(server);
    receiveDigitizer(events);
    assert(![server sendTouch:40 phase:LSBIndigoTouchBegan x:0.5 y:0]);
    receiveTouches(events, bytes, 2);
    assertContact(bytes, 0, 3, 0.5, 0, YES);
    [server close];
    assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
}

int main(void) {
    alarm(15);
    @autoreleasepool {
        edgeGestures();
        int error = 42;
        LSBIndigoHIDServer *server = [[LSBIndigoHIDServer alloc] initWithError:&error];
        assert(server && !error && MACH_PORT_VALID(server.registrationPort));
        mach_port_t events = receivePort();
        audit_token_t token = ownAudit();
        ++token.val[7];
        [server authorizeClientWithAuditToken:&token];
        registerClient(server.registrationPort, events, 0);
        usleep(30000);
        assert(!server.connectedClientPID);
        token = ownAudit();
        [server authorizeClientWithAuditToken:&token];
        registerClient(server.registrationPort, events, 99);
        usleep(30000);
        assert(!server.connectedClientPID);
        registerClient(server.registrationPort, events, 0);
        awaitRegistration(server);
        receiveDigitizer(events);
        uint8_t bytes[4096];
        assert(![server sendTouch:100 phase:LSBIndigoTouchBegan x:0.25 y:0.75]);
        receiveTouches(events, bytes, 2);
        uint32_t identity = uintAt(bytes, 32 + 148 + 20);
        assert(identity && doubleAt(bytes, 32 + 148 + 28) == 0.25 && doubleAt(bytes, 32 + 148 + 36) == 0.75);
        assert(uintAt(bytes, 32 + 148 + 72) == 1 && uintAt(bytes, 32 + 84) == 3);
        assert([server sendTouch:100 phase:LSBIndigoTouchBegan x:0 y:0] == EALREADY);
        assert([server sendTouch:101 phase:LSBIndigoTouchMoved x:0 y:0] == ENOENT);
        assert([server sendTouch:100 phase:LSBIndigoTouchMoved x:NAN y:0] == EINVAL);
        assert(![server sendTouch:100 phase:LSBIndigoTouchMoved x:2 y:-1]);
        receiveTouches(events, bytes, 2);
        assert(uintAt(bytes, 32 + 148 + 20) == identity && doubleAt(bytes, 32 + 148 + 28) == 1 && doubleAt(bytes, 32 + 148 + 36) == 0);
        assert(![server sendTouch:200 phase:LSBIndigoTouchBegan x:0.5 y:0.5]);
        receiveTouches(events, bytes, 3);
        assert(![server sendTouch:100 phase:LSBIndigoTouchEnded x:1 y:0]);
        receiveTouches(events, bytes, 3);
        assert(!uintAt(bytes, 32 + 148 + 72) && uintAt(bytes, 32 + 72));
        assert(![server cancelTouches]);
        receiveTouches(events, bytes, 2);
        assert((uintAt(bytes, 32 + 148 + 24) & 128) && !uintAt(bytes, 32 + 148 + 72));
        // A full guest queue must return promptly and leave tracking unchanged so the view can retry the begin.
        mach_port_limits_t limits = {.mpl_qlimit = 1};
        assert(!mach_port_set_attributes(mach_task_self(), events, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT));
        assert(![server sendTouch:300 phase:LSBIndigoTouchBegan x:0 y:0]);
        assert([server sendTouch:400 phase:LSBIndigoTouchBegan x:0 y:0] == EAGAIN);
        receiveTouches(events, bytes, 2);
        assert(![server sendTouch:400 phase:LSBIndigoTouchBegan x:0 y:0]);
        receiveTouches(events, bytes, 3);
        assert(![server sendTouch:300 phase:LSBIndigoTouchMoved x:0.2 y:0.2]);
        assert([server cancelTouches] == EAGAIN);
        assert([server sendTouch:500 phase:LSBIndigoTouchBegan x:0 y:0] == EBUSY);
        receiveTouches(events, bytes, 3);
        // No additional UI call is needed: draining the guest queue permits the pending cancellation retry.
        receiveTouches(events, bytes, 3);
        assert(!uintAt(bytes, 32 + 72) && !uintAt(bytes, 32 + 148 + 72) && !uintAt(bytes, 32 + 296 + 72));
        assert(![server sendTouch:500 phase:LSBIndigoTouchBegan x:0 y:0]);
        receiveTouches(events, bytes, 2);
        assert(![server sendTouch:500 phase:LSBIndigoTouchMoved x:0.3 y:0.3]);
        assert([server cancelTouches] == EAGAIN);
        [server authorizeClientWithAuditToken:NULL];
        assert(!server.connectedClientPID && [server sendTouch:500 phase:LSBIndigoTouchBegan x:0 y:0] == ENOTCONN);
        mach_port_urefs_t references;
        kern_return_t referenceResult = mach_port_get_refs(mach_task_self(), events, MACH_PORT_RIGHT_SEND, &references);
        assert(referenceResult == KERN_INVALID_RIGHT || references == 0);
        assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
        events = receivePort();
        [server authorizeClientWithAuditToken:&token];
        registerClient(server.registrationPort, events, 0);
        awaitRegistration(server);
        receiveDigitizer(events);
        usleep(60000); // The retired connection's queued cancellation retry must not affect this connection.
        assert(![server sendTouch:600 phase:LSBIndigoTouchBegan x:0.4 y:0.4]);
        receiveTouches(events, bytes, 2);
        assert(uintAt(bytes, 32 + 148 + 72) == 1);
        [server close];
        assert(!server.registrationPort);
        assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
        server = nil;
        // Exercise deallocation without an explicit close too.
        server = [[LSBIndigoHIDServer alloc] initWithError:&error];
        assert(server && !error);
        server = nil;
        digitizerBackpressure();
    }
    puts("IndigoHIDServerTests passed");
    return 0;
}
