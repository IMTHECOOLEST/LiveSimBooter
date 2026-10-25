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
    assert(header->msgh_size == 32 + count * 144 && header->msgh_id == 0 && header->msgh_remote_port == MACH_PORT_NULL);
    assert(uintAt(bytes, 24) == 144 && bytes[28] == count && uintAt(bytes, 32) == 11);
}

int main(void) {
    alarm(15);
    @autoreleasepool {
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
        uint8_t bytes[4096];
        assert(![server sendTouch:100 phase:LSBIndigoTouchBegan x:0.25 y:0.75]);
        receiveTouches(events, bytes, 2);
        uint32_t identity = uintAt(bytes, 32 + 144 + 20);
        assert(identity && doubleAt(bytes, 32 + 144 + 28) == 0.25 && doubleAt(bytes, 32 + 144 + 36) == 0.75);
        assert(uintAt(bytes, 32 + 144 + 72) == 1 && uintAt(bytes, 32 + 84) == 3);
        assert([server sendTouch:100 phase:LSBIndigoTouchBegan x:0 y:0] == EALREADY);
        assert([server sendTouch:101 phase:LSBIndigoTouchMoved x:0 y:0] == ENOENT);
        assert([server sendTouch:100 phase:LSBIndigoTouchMoved x:NAN y:0] == EINVAL);
        assert(![server sendTouch:100 phase:LSBIndigoTouchMoved x:2 y:-1]);
        receiveTouches(events, bytes, 2);
        assert(uintAt(bytes, 32 + 144 + 20) == identity && doubleAt(bytes, 32 + 144 + 28) == 1 && doubleAt(bytes, 32 + 144 + 36) == 0);
        assert(![server sendTouch:200 phase:LSBIndigoTouchBegan x:0.5 y:0.5]);
        receiveTouches(events, bytes, 3);
        assert(![server sendTouch:100 phase:LSBIndigoTouchEnded x:1 y:0]);
        receiveTouches(events, bytes, 3);
        assert(!uintAt(bytes, 32 + 144 + 72) && uintAt(bytes, 32 + 72));
        assert(![server cancelTouches]);
        receiveTouches(events, bytes, 2);
        assert((uintAt(bytes, 32 + 144 + 24) & 128) && !uintAt(bytes, 32 + 144 + 72));
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
        assert(!uintAt(bytes, 32 + 72) && !uintAt(bytes, 32 + 144 + 72) && !uintAt(bytes, 32 + 288 + 72));
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
        usleep(60000); // The retired connection's queued cancellation retry must not affect this connection.
        assert(![server sendTouch:600 phase:LSBIndigoTouchBegan x:0.4 y:0.4]);
        receiveTouches(events, bytes, 2);
        assert(uintAt(bytes, 32 + 144 + 72) == 1);
        [server close];
        assert(!server.registrationPort);
        assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
        server = nil;
        // Exercise deallocation without an explicit close too.
        server = [[LSBIndigoHIDServer alloc] initWithError:&error];
        assert(server && !error);
        server = nil;
    }
    puts("IndigoHIDServerTests passed");
    return 0;
}
