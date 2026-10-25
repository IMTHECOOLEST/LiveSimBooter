#import "../Host/FramebufferServer.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

static uint32_t u32(const void *bytes) { uint32_t value; memcpy(&value, bytes, 4); return value; }
static uint64_t u64(const void *bytes) { uint64_t value; memcpy(&value, bytes, 8); return value; }
static void put32(void *bytes, uint32_t value) { memcpy(bytes, &value, 4); }
static void put64(void *bytes, uint64_t value) { memcpy(bytes, &value, 8); }
static mach_port_t receivePort(void) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    return port;
}
static audit_token_t ownAudit(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    return token;
}
static void makeRequest(uint8_t *bytes, unsigned id, const uint8_t *session, mach_port_t event) {
    memset(bytes, 0, 4096);
    mach_msg_header_t *header = (void *)bytes;
    header->msgh_id = id;
    unsigned delta = event ? 16 : 0;
    header->msgh_size = 76 + delta + (event ? 264 : 0);
    put32(bytes + 24, event ? 1 : 0);
    put64(bytes + 28 + delta, UINT64_C(0x00ace0ace0face01));
    if (session) memcpy(bytes + 36 + delta, session, 16);
    if (event) {
        mach_msg_port_descriptor_t *descriptor = (void *)(bytes + 28);
        *descriptor = (mach_msg_port_descriptor_t){.name = event, .disposition = MACH_MSG_TYPE_MAKE_SEND, .type = MACH_MSG_PORT_DESCRIPTOR};
        put32(bytes + 52 + delta, 1);
        put64(bytes + 56 + delta, UINT64_C(0xfadecafe00007777));
        put32(bytes + 64 + delta, 14);
        put32(bytes + 68 + delta, 264);
        memcpy(bytes + 80 + delta, "CAWindowServer", 15);
    }
}
static void transact(LSBFramebufferServer *server, uint8_t *bytes) {
    mach_port_t reply = receivePort();
    mach_msg_header_t *header = (void *)bytes;
    header->msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    header->msgh_remote_port = server.registrationPort;
    header->msgh_local_port = reply;
    assert(!mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, header->msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL));
    memset(bytes, 0, 4096);
    assert(!mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, 4096, reply, 2000, MACH_PORT_NULL));
    assert(!mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(header->msgh_size >= 76 && u64(bytes + 28) == UINT64_C(0x00ace0ace0face01));
}
static void assertError(uint8_t *bytes, int error) {
    assert(u32(bytes + 52) == 1 && u32(bytes + 64) == 17);
    assert(u64(bytes + 72 + 140) == 1001 && (int64_t)u64(bytes + 72 + 148) == error);
}
static IOSurfaceRef createSurface(void) CF_RETURNS_RETAINED;
static IOSurfaceRef createSurface(void) {
    NSDictionary *properties = @{(__bridge NSString *)kIOSurfaceWidth: @1290, (__bridge NSString *)kIOSurfaceHeight: @2796,
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4, (__bridge NSString *)kIOSurfacePixelFormat: @(0x42475241),
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, 1290 * 4))};
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    assert(surface && !IOSurfaceLock(surface, 0, NULL));
    memcpy(IOSurfaceGetBaseAddress(surface), "\x11\x22\x33\xff", 4);
    assert(!IOSurfaceUnlock(surface, 0, NULL));
    return surface;
}
static uint8_t *singleItem(uint8_t *bytes, unsigned type, unsigned delta) {
    put32(bytes + 52 + delta, 1);
    put64(bytes + 56 + delta, UINT64_C(0xfadecafe00007777));
    put32(bytes + 64 + delta, type);
    put32(bytes + 68 + delta, 264);
    ((mach_msg_header_t *)bytes)->msgh_size += 264;
    return bytes + 72 + delta;
}
static void presentRequest(uint8_t *bytes, const uint8_t *session, IOSurfaceRef surface, unsigned operation, mach_port_t fence) {
    makeRequest(bytes, 11, session, MACH_PORT_NULL);
    unsigned count = fence ? 2 : 1, delta = fence ? 24 : 16;
    memset(bytes + 28, 0, 48 + delta);
    put32(bytes + 24, count);
    put64(bytes + 28 + delta, UINT64_C(0x00ace0ace0face01));
    memcpy(bytes + 36 + delta, session, 16);
    ((mach_msg_header_t *)bytes)->msgh_size = 76 + delta;
    mach_msg_port_descriptor_t *ports = (void *)(bytes + 28);
    ports[0] = (mach_msg_port_descriptor_t){.name = IOSurfaceCreateMachPort(surface), .disposition = MACH_MSG_TYPE_MOVE_SEND,
                                          .type = MACH_MSG_PORT_DESCRIPTOR};
    if (fence) ports[1] = (mach_msg_port_descriptor_t){.name = fence, .disposition = MACH_MSG_TYPE_MOVE_RECEIVE, .type = MACH_MSG_PORT_DESCRIPTOR};
    uint8_t *payload = singleItem(bytes, 10, delta);
    put64(payload, operation); put64(payload + 16, 0x18);
    uint32_t rectangle[] = {0, 0, 1290, 2796};
    memcpy(payload + 24, rectangle, 16); memcpy(payload + 40, rectangle, 16);
    put32(payload + 56, 1); put32(payload + 60, 1); put32(payload + 64, IOSurfaceGetID(surface));
    put32(payload + 68, 0); put32(payload + 72, fence ? 1 : UINT32_MAX);
}

// Most wire tests finish immediately; separate lifetime tests below deliberately retain completion.
static LSBFramebufferPresent immediateHandler(int (^handler)(IOSurfaceRef, CGRect)) {
    return ^(IOSurfaceRef surface, CGRect rectangle, LSBFramebufferCompletion completion) { completion(handler(surface, rectangle)); };
}

static void cancellationTests(void) {
    int error;
    LSBFramebufferServer *server = [[LSBFramebufferServer alloc] initWithError:&error];
    assert(server && !error);
    __block _Atomic(unsigned) copied = 0;
    server.frameHandler = immediateHandler(^int(IOSurfaceRef surface, CGRect dirtyRect) {
        (void)surface; assert(CGRectEqualToRect(dirtyRect, CGRectMake(0, 0, 1290, 2796)));
        atomic_fetch_add(&copied, 1); return 0;
    });
    audit_token_t token = ownAudit();
    [server authorizeClientWithAuditToken:&token];
    mach_port_t events = receivePort();
    uint8_t bytes[4096], session[16];
    makeRequest(bytes, 1, NULL, events); transact(server, bytes); memcpy(session, bytes + 72, 16);
    makeRequest(bytes, 9, session, MACH_PORT_NULL);
    uint8_t *chain = singleItem(bytes, 7, 0);
    put32(chain, 1290); put32(chain + 4, 2796); put32(chain + 8, 1); put32(chain + 28, 2);
    transact(server, bytes);
    assert(u32(bytes + 64) == 7 && u32(bytes + 84) == 1);
    IOSurfaceRef surface = createSurface();
    presentRequest(bytes, session, surface, 1, MACH_PORT_NULL);
    put64(bytes + 72 + 16 + 8, UINT64_MAX);
    transact(server, bytes);
    assertError(bytes, ENOTSUP); // A request beyond the supported scheduling horizon must fail explicitly.
    dispatch_semaphore_t mainBlocked = dispatch_semaphore_create(0), unblockMain = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        dispatch_semaphore_signal(mainBlocked);
        assert(!dispatch_semaphore_wait(unblockMain, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)));
    });
    assert(!dispatch_semaphore_wait(mainBlocked, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
    mach_port_t fence = receivePort(), firstReply = receivePort();
    assert(!mach_port_insert_right(mach_task_self(), fence, fence, MACH_MSG_TYPE_MAKE_SEND));
    presentRequest(bytes, session, surface, 1, fence);
    mach_msg_header_t *header = (void *)bytes;
    header->msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    header->msgh_remote_port = server.registrationPort; header->msgh_local_port = firstReply;
    assert(!mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, header->msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL));
    presentRequest(bytes, session, surface, 2, MACH_PORT_NULL);
    transact(server, bytes); assertError(bytes, EAGAIN);
    mach_port_type_t portType;
    assert(!mach_port_type(mach_task_self(), fence, &portType) && (portType & MACH_PORT_TYPE_SEND));
    [server authorizeClientWithAuditToken:NULL];
    memset(bytes, 0, sizeof(bytes));
    assert(!mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(bytes), firstReply, 1000, MACH_PORT_NULL));
    assertError(bytes, ECANCELED);
    assert(!mach_port_type(mach_task_self(), fence, &portType) && (portType & MACH_PORT_TYPE_DEAD_NAME));
    assert(!mach_port_deallocate(mach_task_self(), fence));
    assert(!mach_port_mod_refs(mach_task_self(), firstReply, MACH_PORT_RIGHT_RECEIVE, -1));
    [server authorizeClientWithAuditToken:&token];
    makeRequest(bytes, 1, NULL, events); transact(server, bytes);
    assert(u32(bytes + 64) == 18); memcpy(session, bytes + 72, 16);
    makeRequest(bytes, 9, session, MACH_PORT_NULL);
    chain = singleItem(bytes, 7, 0);
    put32(chain, 1290); put32(chain + 4, 2796); put32(chain + 8, 1);
    transact(server, bytes);
    assert(u32(bytes + 64) == 7);
    unsigned nextChain = u32(bytes + 84);
    presentRequest(bytes, session, surface, 3, MACH_PORT_NULL);
    put32(bytes + 72 + 16 + 60, nextChain);
    transact(server, bytes);
    assertError(bytes, EAGAIN); // A cancelled block still occupies the one-main-work slot until main drains it.
    dispatch_semaphore_signal(unblockMain);
    dispatch_sync(dispatch_get_main_queue(), ^{ assert(!atomic_load(&copied)); });
    [server close];
    assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
    CFRelease(surface);
}

static void rawTests(void) {
    int error;
    LSBFramebufferServer *server = [[LSBFramebufferServer alloc] initWithError:&error];
    assert(server && !error);
    audit_token_t token = ownAudit();
    audit_token_t wrong = token;
    ++wrong.val[7];
    [server authorizeClientWithAuditToken:&wrong];
    mach_port_t events = receivePort();
    uint8_t bytes[4096], session[16];
    makeRequest(bytes, 1, NULL, events);
    transact(server, bytes);
    assertError(bytes, EPERM);
    assert(!server.connectedClientPID);
    [server authorizeClientWithAuditToken:&token];
    makeRequest(bytes, 1, NULL, events);
    put64(bytes + 44, 123); // Wrong envelope magic; right still must be cleaned up.
    transact(server, bytes);
    assertError(bytes, EINVAL);
    makeRequest(bytes, 1, NULL, events);
    transact(server, bytes);
    assert(u32(bytes + 64) == 18);
    memcpy(session, bytes + 72, 16);
    for (unsigned i = 0; i < 500 && !server.connectedClientPID; ++i) usleep(1000); // Publication follows successful reply delivery.
    assert(server.connectedClientPID == getpid());
    makeRequest(bytes, 1, NULL, events);
    transact(server, bytes);
    assertError(bytes, EALREADY);
    makeRequest(bytes, 2, session, MACH_PORT_NULL);
    bytes[36] ^= 1;
    transact(server, bytes);
    assertError(bytes, ENOTCONN);
    makeRequest(bytes, 2, session, MACH_PORT_NULL);
    transact(server, bytes);
    assert(u32(bytes + 52) == 2 && u32(bytes + 64) == 4 && u32(bytes + 328) == 5);
    assert(!strcmp((char *)bytes + 72, "PurpleMain"));
    assert(u32(bytes + 72 + 212) == 1290 && u32(bytes + 72 + 216) == 2796);
    makeRequest(bytes, 99, session, MACH_PORT_NULL);
    transact(server, bytes);
    assertError(bytes, ENOTSUP);
    [server authorizeClientWithAuditToken:NULL];
    assert(!server.connectedClientPID);
    makeRequest(bytes, 2, session, MACH_PORT_NULL);
    transact(server, bytes);
    assertError(bytes, EPERM);
    mach_port_urefs_t references;
    kern_return_t result = mach_port_get_refs(mach_task_self(), events, MACH_PORT_RIGHT_SEND, &references);
    for (unsigned i = 0; i < 500 && result == KERN_SUCCESS && references; ++i) {
        usleep(1000); // The dispatch source's cancellation handler releases its separate watch uref asynchronously.
        result = mach_port_get_refs(mach_task_self(), events, MACH_PORT_RIGHT_SEND, &references);
    }
    assert(result == KERN_INVALID_RIGHT || !references);
    assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
    [server close];
    assert(!server.registrationPort);
}

static void delayedCancellationTest(BOOL abandonServer) {
    int error;
    LSBFramebufferServer *server = [[LSBFramebufferServer alloc] initWithError:&error];
    assert(server && !error);
    __block _Atomic(unsigned) copied = 0;
    server.frameHandler = immediateHandler(^int(IOSurfaceRef surface, CGRect dirtyRect) {
        (void)surface; assert(CGRectEqualToRect(dirtyRect, CGRectMake(0, 0, 1290, 2796)));
        atomic_fetch_add(&copied, 1); return 0;
    });
    audit_token_t token = ownAudit();
    [server authorizeClientWithAuditToken:&token];
    mach_port_t events = receivePort(), firstReply = receivePort(), fence = receivePort();
    assert(!mach_port_insert_right(mach_task_self(), fence, fence, MACH_MSG_TYPE_MAKE_SEND));
    uint8_t bytes[4096], session[16];
    makeRequest(bytes, 1, NULL, events); transact(server, bytes); memcpy(session, bytes + 72, 16);
    makeRequest(bytes, 9, session, MACH_PORT_NULL);
    uint8_t *chain = singleItem(bytes, 7, 0);
    put32(chain, 1290); put32(chain + 4, 2796); put32(chain + 8, 1); put32(chain + 28, 2);
    transact(server, bytes);
    assert(u32(bytes + 64) == 7 && u32(bytes + 84) == 1 && u32(bytes + 100) == 2);
    IOSurfaceRef surface = createSurface();
    presentRequest(bytes, session, surface, 1, fence);
    mach_timebase_info_data_t timebase;
    assert(!mach_timebase_info(&timebase));
    uint64_t target = (uint64_t)((__uint128_t)mach_absolute_time() * timebase.numer / timebase.denom) + 250000000;
    put64(bytes + 72 + 24 + 8, target);
    mach_msg_header_t *header = (void *)bytes;
    header->msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    header->msgh_remote_port = server.registrationPort; header->msgh_local_port = firstReply;
    assert(!mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, header->msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL));
    if (abandonServer) {
        makeRequest(bytes, 2, session, MACH_PORT_NULL); // Ensure the delayed frame was accepted before releasing the last server reference.
        transact(server, bytes);
        assert(u32(bytes + 52) == 2);
        __weak LSBFramebufferServer *weakServer = server;
        server = nil;
        for (unsigned i = 0; i < 500 && weakServer; ++i) usleep(1000);
        assert(!weakServer);
    } else {
        makeRequest(bytes, 18, session, MACH_PORT_NULL);
        uint8_t *cancel = singleItem(bytes, 22, 0);
        put32(cancel, 1); put32(cancel + 4, 1);
        transact(server, bytes);
        assert(!u32(bytes + 52));
    }
    memset(bytes, 0, sizeof(bytes));
    assert(!mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(bytes), firstReply, 1000, MACH_PORT_NULL));
    if (abandonServer) assert(header->msgh_id == MACH_NOTIFY_SEND_ONCE);
    else assertError(bytes, ECANCELED);
    mach_port_type_t type;
    assert(!mach_port_type(mach_task_self(), fence, &type) && (type & MACH_PORT_TYPE_DEAD_NAME));
    usleep(300000);
    assert(!atomic_load(&copied));
    [server close];
    assert(!mach_port_deallocate(mach_task_self(), fence));
    assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_mod_refs(mach_task_self(), firstReply, MACH_PORT_RIGHT_RECEIVE, -1));
    CFRelease(surface);
}

static uint32_t createRawChain(LSBFramebufferServer *server, uint8_t *bytes, const uint8_t *session) {
    makeRequest(bytes, 9, session, MACH_PORT_NULL);
    uint8_t *chain = singleItem(bytes, 7, 0);
    put32(chain, 1290); put32(chain + 4, 2796); put32(chain + 8, 1);
    transact(server, bytes);
    assert(u32(bytes + 64) == 7 && u32(bytes + 84));
    return u32(bytes + 84);
}

static void presentRectangle(LSBFramebufferServer *server, uint8_t *bytes, const uint8_t *session, IOSurfaceRef surface,
                             unsigned operation, uint32_t chain, const uint32_t *source, const uint32_t *destination, int error) {
    presentRequest(bytes, session, surface, operation, MACH_PORT_NULL);
    uint8_t *present = bytes + 72 + 16;
    put64(present + 16, 8); // No event callback; the synchronous reply follows the actual frame handler.
    put32(present + 60, chain);
    memcpy(present + 24, source, 16); memcpy(present + 40, destination, 16);
    transact(server, bytes);
    if (error) assertError(bytes, error);
    else assert(!u32(bytes + 52));
}

static void sendDeferred(LSBFramebufferServer *server, uint8_t *bytes, mach_port_t reply) {
    mach_msg_header_t *header = (void *)bytes;
    header->msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    header->msgh_remote_port = server.registrationPort;
    header->msgh_local_port = reply;
    assert(!mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, header->msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL));
}

static void assertNoMessage(mach_port_t port) {
    uint8_t bytes[4096] = {0};
    assert(mach_msg((void *)bytes, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(bytes), port, 0, MACH_PORT_NULL) == MACH_RCV_TIMED_OUT);
}

static void assertFence(mach_port_t fence, mach_port_type_t expected) {
    mach_port_type_t type;
    assert(!mach_port_type(mach_task_self(), fence, &type) && (type & expected));
}

static void asynchronousCompletionTests(void) {
    int error;
    LSBFramebufferServer *server = [[LSBFramebufferServer alloc] initWithError:&error];
    assert(server && !error);
    dispatch_semaphore_t started = dispatch_semaphore_create(0);
    __block LSBFramebufferCompletion heldCompletion;
    __block IOSurfaceRef borrowedSurface;
    __block unsigned calls = 0;
    server.frameHandler = ^(IOSurfaceRef surface, CGRect rectangle, LSBFramebufferCompletion completion) {
        assert([NSThread isMainThread] && CGRectEqualToRect(rectangle, CGRectMake(0, 0, 1290, 2796)));
        borrowedSurface = surface;
        heldCompletion = completion;
        ++calls;
        dispatch_semaphore_signal(started);
    };
    audit_token_t token = ownAudit();
    [server authorizeClientWithAuditToken:&token];
    mach_port_t events = receivePort(), reply = receivePort(), fence = receivePort();
    assert(!mach_port_insert_right(mach_task_self(), fence, fence, MACH_MSG_TYPE_MAKE_SEND));
    uint8_t bytes[4096], session[16];
    makeRequest(bytes, 1, NULL, events); transact(server, bytes); memcpy(session, bytes + 72, 16);
    uint32_t chain = createRawChain(server, bytes, session);
    IOSurfaceRef surface = createSurface();
    presentRequest(bytes, session, surface, 1, fence);
    put32(bytes + 72 + 24 + 60, chain);
    sendDeferred(server, bytes, reply);
    assert(!dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
    CFRelease(surface); // Only the server owns the borrowed surface while the asynchronous consumer still reads it.
    __block LSBFramebufferCompletion firstCompletion;
    dispatch_sync(dispatch_get_main_queue(), ^{ firstCompletion = heldCompletion; assert(calls == 1); });
    assertNoMessage(reply);
    assertFence(fence, MACH_PORT_TYPE_SEND);

    [server authorizeClientWithAuditToken:NULL];
    makeRequest(bytes, 2, session, MACH_PORT_NULL); transact(server, bytes); assertError(bytes, EPERM);
    assertNoMessage(reply); // Revocation cannot acknowledge producer reuse before the consumer finishes reading.
    assertFence(fence, MACH_PORT_TYPE_SEND);
    dispatch_sync(dispatch_get_main_queue(), ^{
        assert(borrowedSurface && !IOSurfaceLock(borrowedSurface, kIOSurfaceLockReadOnly, NULL));
        assert(!memcmp(IOSurfaceGetBaseAddress(borrowedSurface), "\x11\x22\x33\xff", 4));
        assert(!IOSurfaceUnlock(borrowedSurface, kIOSurfaceLockReadOnly, NULL));
    });
    [server authorizeClientWithAuditToken:&token];
    makeRequest(bytes, 1, NULL, events); transact(server, bytes); memcpy(session, bytes + 72, 16);
    chain = createRawChain(server, bytes, session);
    surface = createSurface();
    const uint32_t full[] = {0, 0, 1290, 2796}, partial[] = {10, 20, 30, 40};
    presentRectangle(server, bytes, session, surface, 2, chain, full, full, EAGAIN);
    firstCompletion(0); // Completion may come from a worker queue, including after session replacement.
    memset(bytes, 0, sizeof(bytes));
    assert(!mach_msg((void *)bytes, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(bytes), reply, 1000, MACH_PORT_NULL));
    assertError(bytes, ECANCELED);
    assertFence(fence, MACH_PORT_TYPE_DEAD_NAME);
    assertNoMessage(events); // A previous generation cannot report successful presentation to the new client.
    firstCompletion(EIO);
    presentRectangle(server, bytes, session, surface, 3, chain, partial, partial, ENODATA);

    mach_port_t secondFence = receivePort();
    assert(!mach_port_insert_right(mach_task_self(), secondFence, secondFence, MACH_MSG_TYPE_MAKE_SEND));
    presentRequest(bytes, session, surface, 4, secondFence);
    put32(bytes + 72 + 24 + 60, chain);
    sendDeferred(server, bytes, reply);
    assert(!dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
    __block LSBFramebufferCompletion secondCompletion;
    dispatch_sync(dispatch_get_main_queue(), ^{ secondCompletion = heldCompletion; assert(calls == 2); });
    firstCompletion(0); // An old duplicate must not clear the new in-flight slot or its fence.
    presentRectangle(server, bytes, session, surface, 5, chain, full, full, EAGAIN);
    assertNoMessage(reply);
    assertFence(secondFence, MACH_PORT_TYPE_SEND);
    secondCompletion(0);
    memset(bytes, 0, sizeof(bytes));
    assert(!mach_msg((void *)bytes, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(bytes), reply, 1000, MACH_PORT_NULL));
    assert(!u32(bytes + 52));
    assertFence(secondFence, MACH_PORT_TYPE_DEAD_NAME);
    memset(bytes, 0, sizeof(bytes));
    assert(!mach_msg((void *)bytes, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(bytes), events, 1000, MACH_PORT_NULL));
    assert(((mach_msg_header_t *)bytes)->msgh_id == 12 && u32(bytes + 64) == 11 && u64(bytes + 72) == 4);
    dispatch_apply(8, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(size_t index) { (void)index; secondCompletion(EIO); });
    makeRequest(bytes, 2, session, MACH_PORT_NULL); transact(server, bytes); // Drain the server queue before checking duplicate effects.
    assertNoMessage(reply); assertNoMessage(events);
    [server close];
    dispatch_sync(dispatch_get_main_queue(), ^{ heldCompletion = nil; borrowedSurface = NULL; });
    firstCompletion = nil; secondCompletion = nil;
    assert(!mach_port_deallocate(mach_task_self(), fence) && !mach_port_deallocate(mach_task_self(), secondFence));
    assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1));
    CFRelease(surface);
}

static void dirtyRectangleTests(void) {
    int error;
    LSBFramebufferServer *server = [[LSBFramebufferServer alloc] initWithError:&error];
    assert(server && !error);
    __block unsigned copied = 0;
    __block CGRect observed = CGRectZero;
    __block BOOL reject = NO;
    server.frameHandler = immediateHandler(^int(IOSurfaceRef surface, CGRect dirtyRect) {
        assert(surface && [NSThread isMainThread]);
        observed = dirtyRect;
        ++copied;
        return reject ? EIO : 0;
    });
    audit_token_t token = ownAudit();
    [server authorizeClientWithAuditToken:&token];
    mach_port_t events = receivePort();
    uint8_t bytes[4096], session[16];
    makeRequest(bytes, 1, NULL, events); transact(server, bytes); memcpy(session, bytes + 72, 16);
    IOSurfaceRef surface = createSurface();
    const uint32_t full[] = {0, 0, 1290, 2796}, partial[] = {597, 1350, 96, 96}, edge[] = {1289, 2795, 1, 1};
    unsigned operation = 0, expectedCopies = 0;
    uint32_t chain = 0;
    for (unsigned phase = 0; phase < 3; ++phase) {
        if (phase == 1) {
            makeRequest(bytes, 13, session, MACH_PORT_NULL);
            uint8_t *destroy = singleItem(bytes, 7, 0);
            put32(destroy + 8, 1); put32(destroy + 12, chain);
            transact(server, bytes);
            assert(!u32(bytes + 52));
        } else if (phase == 2) {
            [server authorizeClientWithAuditToken:NULL];
            [server authorizeClientWithAuditToken:&token];
            makeRequest(bytes, 1, NULL, events); transact(server, bytes); memcpy(session, bytes + 72, 16);
        }
        chain = createRawChain(server, bytes, session);
        presentRectangle(server, bytes, session, surface, ++operation, chain, partial, partial, ENODATA);
        dispatch_sync(dispatch_get_main_queue(), ^{ assert(copied == expectedCopies); });
        if (!phase) {
            // Failed full copies cannot initialize the image on which a subsequent partial update depends.
            dispatch_sync(dispatch_get_main_queue(), ^{ reject = YES; });
            presentRectangle(server, bytes, session, surface, ++operation, chain, full, full, EIO);
            ++expectedCopies;
            dispatch_sync(dispatch_get_main_queue(), ^{ reject = NO; });
            presentRectangle(server, bytes, session, surface, ++operation, chain, partial, partial, ENODATA);
        }
        presentRectangle(server, bytes, session, surface, ++operation, chain, full, full, 0);
        ++expectedCopies;
        dispatch_sync(dispatch_get_main_queue(), ^{
            assert(copied == expectedCopies && CGRectEqualToRect(observed, CGRectMake(0, 0, 1290, 2796)));
        });
        presentRectangle(server, bytes, session, surface, ++operation, chain, partial, partial, 0);
        ++expectedCopies;
        dispatch_sync(dispatch_get_main_queue(), ^{
            assert(copied == expectedCopies && CGRectEqualToRect(observed, CGRectMake(597, 1350, 96, 96)));
        });
        if (!phase) {
            const uint32_t invalid[][4] = {
                {0, 0, 0, 1}, {0, 0, 1, 0}, {0, 0, 1291, 1}, {0, 0, 1, 2797}, {1290, 0, 1, 1}, {0, 2796, 1, 1},
                {UINT32_MAX, 0, 2, 1}, {0, UINT32_MAX, 1, 2}, {1, 0, UINT32_MAX, 1}, {0, 1, 1, UINT32_MAX}
            };
            for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
                presentRectangle(server, bytes, session, surface, ++operation, chain, invalid[i], partial, EINVAL);
                presentRectangle(server, bytes, session, surface, ++operation, chain, partial, invalid[i], EINVAL);
            }
            const uint32_t translated[] = {598, 1350, 96, 96}, scaled[] = {597, 1350, 48, 48};
            presentRectangle(server, bytes, session, surface, ++operation, chain, partial, translated, ENOTSUP);
            presentRectangle(server, bytes, session, surface, ++operation, chain, partial, scaled, ENOTSUP);
            dispatch_sync(dispatch_get_main_queue(), ^{ assert(copied == expectedCopies); });
            presentRectangle(server, bytes, session, surface, ++operation, chain, edge, edge, 0);
            ++expectedCopies;
            dispatch_sync(dispatch_get_main_queue(), ^{
                assert(copied == expectedCopies && CGRectEqualToRect(observed, CGRectMake(1289, 2795, 1, 1)));
            });
        }
    }
    [server close];
    assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
    CFRelease(surface);
}

static void stockClientTests(void) {
    const char *path = "/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A/Frameworks/SimFramebuffer.framework/Versions/A/SimFramebuffer";
    void *library = dlopen(path, RTLD_LOCAL | RTLD_NOW);
    if (!library) { fprintf(stderr, "Framebuffer stock-client test unavailable: %s\n", dlerror()); abort(); }
#define API(type, name) type name = (type)dlsym(library, #name); assert(name)
    typedef void (*SetPort)(mach_port_t);
    typedef void (*SetEnabled)(bool);
    typedef CFTypeRef (*Create)(CFAllocatorRef, CFStringRef);
    typedef bool (*Connect)(CFTypeRef, CFErrorRef *);
    typedef CFArrayRef (*Displays)(CFTypeRef, CFErrorRef *);
    typedef CFStringRef (*DisplayName)(CFTypeRef);
    typedef uint32_t (*DisplayID)(CFTypeRef);
    typedef void (*Mode)(CFTypeRef, void *);
    typedef CFTypeRef (*Chain)(CFTypeRef, CGSize, uint32_t, CFDictionaryRef, CFErrorRef *);
    typedef bool (*Begin)(CFTypeRef, uint64_t);
    typedef bool (*AddSurface)(CFTypeRef, IOSurfaceRef, CGRect, CGRect, uint64_t, uint64_t, CFErrorRef *);
    typedef bool (*Submit)(CFTypeRef, CFErrorRef *);
    typedef bool (*Acquire)(CFTypeRef, IOSurfaceRef, dispatch_time_t);
    typedef void (*SetCallback)(CFTypeRef, void *, dispatch_queue_t, void (^)(CFErrorRef, CFTypeRef, uint64_t, uint64_t, void *));
    API(SetPort, _SFBSetServerPort);
    API(SetEnabled, _SFBSetFramebufferServerEnabled);
    API(Create, SFBConnectionCreate);
    API(Connect, SFBConnectionConnect);
    API(Displays, SFBConnectionCopyDisplays);
    API(DisplayName, SFBDisplayGetName);
    API(DisplayID, SFBDisplayGetID);
    API(Mode, SFBDisplayGetCurrentMode);
    API(Chain, SFBDisplayCreateSwapchain);
    API(Begin, SFBSwapchainSwapBegin);
    API(AddSurface, SFBSwapchainSwapAddSurface);
    API(Submit, SFBSwapchainSwapSubmit);
    API(SetCallback, SFBSwapchainSwapSetCallback);
    API(Acquire, SFBSwapchainAcquireSurfaceFence);
    int error;
    LSBFramebufferServer *server = [[LSBFramebufferServer alloc] initWithError:&error];
    assert(server && !error);
    __block _Atomic(unsigned) presented = 0;
    __block CFTypeRef activeChain = NULL;
    __block uint64_t earliestCopy = 0;
    __block CGRect expectedRectangle = CGRectMake(0, 0, 1290, 2796);
    server.frameHandler = immediateHandler(^int(IOSurfaceRef surface, CGRect dirtyRect) {
        assert([NSThread isMainThread]);
        assert(CGRectEqualToRect(dirtyRect, expectedRectangle));
        assert(mach_absolute_time() >= earliestCopy);
        assert(activeChain && !SFBSwapchainAcquireSurfaceFence(activeChain, surface, DISPATCH_TIME_NOW));
        assert(!IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL));
        assert(!memcmp(IOSurfaceGetBaseAddress(surface), "\x11\x22\x33\xff", 4));
        assert(!IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL));
        atomic_fetch_add(&presented, 1);
        return 0;
    });
    audit_token_t token = ownAudit();
    [server authorizeClientWithAuditToken:&token];
    _SFBSetServerPort(server.registrationPort);
    _SFBSetFramebufferServerEnabled(true);
    CFTypeRef connection = SFBConnectionCreate(kCFAllocatorDefault, CFSTR("CAWindowServer"));
    assert(connection);
    CFErrorRef failure = NULL;
    if (!SFBConnectionConnect(connection, &failure)) { CFShow(failure); abort(); }
    CFArrayRef displays = SFBConnectionCopyDisplays(connection, &failure);
    assert(displays && CFArrayGetCount(displays) == 1);
    CFTypeRef display = CFArrayGetValueAtIndex(displays, 0);
    assert(CFEqual(SFBDisplayGetName(display), CFSTR("LCD")) && SFBDisplayGetID(display) == 1);
    uint8_t mode[35];
    SFBDisplayGetCurrentMode(display, mode);
    assert(u32(mode + 4) == 1290 && u32(mode + 8) == 2796 && u32(mode + 12) == 3);
    NSDictionary *options = @{ @"UseFences": @YES }; // Actual SFBDisplayCreateSwapchain option string at provider cfstring 0x18b88.
    CFTypeRef chain = SFBDisplayCreateSwapchain(display, CGSizeMake(1290, 2796), 0, (__bridge CFDictionaryRef)options, &failure);
    if (!chain) { CFShow(failure); abort(); }
    activeChain = chain;
    IOSurfaceRef surface = createSurface();
    dispatch_semaphore_t completed = dispatch_semaphore_create(0);
    CGRect rectangle = CGRectMake(0, 0, 1290, 2796);
    assert(SFBSwapchainSwapBegin(chain, 0));
    SFBSwapchainSwapSetCallback(chain, (void *)123, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
        ^(CFErrorRef callbackError, CFTypeRef callbackChain, uint64_t desired, uint64_t completedAt, void *context) {
            assert(!callbackError && callbackChain == chain && !desired && completedAt > 0);
            assert(context == (void *)123);
            assert(atomic_load(&presented) == 1);
            dispatch_semaphore_signal(completed);
        });
    assert(SFBSwapchainSwapAddSurface(chain, surface, rectangle, rectangle, 0, 0, &failure));
    if (!SFBSwapchainSwapSubmit(chain, &failure)) { CFShow(failure); abort(); }
    assert(SFBSwapchainAcquireSurfaceFence(chain, surface, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
    assert(!dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)));
    assert(atomic_load(&presented) == 1);
    // Match the stock backboardd update: the unchanged area belongs to the preceding successful full frame.
    CGRect partial = CGRectMake(597, 1350, 96, 96);
    dispatch_sync(dispatch_get_main_queue(), ^{ expectedRectangle = partial; });
    assert(SFBSwapchainSwapBegin(chain, 0));
    SFBSwapchainSwapSetCallback(chain, (void *)789, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
        ^(CFErrorRef callbackError, CFTypeRef callbackChain, uint64_t desired, uint64_t completedAt, void *context) {
            assert(!callbackError && callbackChain == chain && !desired && completedAt > 0);
            assert(context == (void *)789 && atomic_load(&presented) == 2);
            dispatch_semaphore_signal(completed);
        });
    assert(SFBSwapchainSwapAddSurface(chain, surface, partial, partial, 0, 0, &failure));
    if (!SFBSwapchainSwapSubmit(chain, &failure)) { CFShow(failure); abort(); }
    assert(SFBSwapchainAcquireSurfaceFence(chain, surface, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
    assert(!dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)));
    assert(atomic_load(&presented) == 2);
    dispatch_sync(dispatch_get_main_queue(), ^{ expectedRectangle = rectangle; });
    CFRelease(chain);
    activeChain = NULL;
    chain = SFBDisplayCreateSwapchain(display, CGSizeMake(1290, 2796), 2, (__bridge CFDictionaryRef)options, &failure);
    assert(chain); // Releasing the first client object must free its server-side slot through message ID 13.
    activeChain = chain;
    mach_timebase_info_data_t timebase;
    assert(!mach_timebase_info(&timebase));
    uint64_t target = mach_absolute_time() + (uint64_t)((__uint128_t)100000000 * timebase.denom / timebase.numer);
    earliestCopy = target;
    assert(SFBSwapchainSwapBegin(chain, target));
    SFBSwapchainSwapSetCallback(chain, (void *)456, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
        ^(CFErrorRef callbackError, CFTypeRef callbackChain, uint64_t desired, uint64_t completedAt, void *context) {
            assert(!callbackError && callbackChain == chain && desired <= target && target - desired <= 1 && completedAt >= target);
            assert(context == (void *)456 && atomic_load(&presented) == 3);
            dispatch_semaphore_signal(completed);
        });
    assert(SFBSwapchainSwapAddSurface(chain, surface, rectangle, rectangle, 0, 0, &failure));
    if (!SFBSwapchainSwapSubmit(chain, &failure)) { CFShow(failure); abort(); }
    assert(mach_absolute_time() >= target && SFBSwapchainAcquireSurfaceFence(chain, surface, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)));
    assert(!dispatch_semaphore_wait(completed, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC)));
    CFRelease(surface);
    CFRelease(chain);
    CFRelease(displays);
    CFRelease(connection);
    [server close];
#undef API
}

static void diagnosticTests(void) {
    int error;
    LSBFramebufferServer *server = [[LSBFramebufferServer alloc] initWithError:&error];
    assert(server && !error);
    __block _Atomic(bool) rejectCopy = false;
    server.frameHandler = immediateHandler(^int(IOSurfaceRef surface, CGRect dirtyRect) {
        assert(surface && CGRectEqualToRect(dirtyRect, CGRectMake(0, 0, 1290, 2796)));
        return atomic_load(&rejectCopy) ? EIO : 0;
    });
    IOSurfaceRef surface = createSurface();
    audit_token_t token = ownAudit();
    FILE *capture = tmpfile();
    assert(capture);
    fflush(stderr);
    int original = dup(STDERR_FILENO);
    assert(original >= 0 && dup2(fileno(capture), STDERR_FILENO) == STDERR_FILENO);
    for (unsigned generation = 0; generation < 2; ++generation) {
        [server authorizeClientWithAuditToken:&token];
        mach_port_t events = receivePort();
        uint8_t bytes[4096], session[16];
        makeRequest(bytes, 1, NULL, events); transact(server, bytes); memcpy(session, bytes + 72, 16);
        makeRequest(bytes, 9, session, MACH_PORT_NULL);
        uint8_t *chain = singleItem(bytes, 7, 0);
        put32(chain, 1290); put32(chain + 4, 2796); put32(chain + 8, 1);
        transact(server, bytes);
        assert(u32(bytes + 64) == 7);
        uint32_t chainID = u32(bytes + 84);
        for (unsigned operation = 1; operation <= 2; ++operation) {
            atomic_store(&rejectCopy, operation == 1);
            presentRequest(bytes, session, surface, operation, MACH_PORT_NULL);
            uint8_t *present = bytes + 72 + 16;
            put64(present + 16, 8); // No asynchronous callback; only the actual snapshot result is relevant here.
            put32(present + 60, chainID);
            transact(server, bytes);
            if (atomic_load(&rejectCopy)) assertError(bytes, EIO);
            else assert(!u32(bytes + 52));
        }
        for (unsigned attempt = 0; attempt < 40; ++attempt) {
            makeRequest(bytes, 99, session, MACH_PORT_NULL); transact(server, bytes); assertError(bytes, ENOTSUP);
        }
        [server authorizeClientWithAuditToken:NULL];
        makeRequest(bytes, 2, session, MACH_PORT_NULL); transact(server, bytes); assertError(bytes, EPERM);
        assert(!mach_port_mod_refs(mach_task_self(), events, MACH_PORT_RIGHT_RECEIVE, -1));
    }
    [server close];
    fflush(stderr);
    assert(dup2(original, STDERR_FILENO) == STDERR_FILENO && !close(original));
    rewind(capture);
    unsigned failures = 0, completions = 0, rejected = 0;
    char line[512];
    while (fgets(line, sizeof(line), capture)) {
        if (strstr(line, "snapshot failed native=")) { ++failures; assert(strstr(line, "operation=1 error=5")); }
        if (strstr(line, "snapshot complete native=")) { ++completions; assert(strstr(line, "operation=2 frames=1")); }
        if (strstr(line, "reject id=99")) ++rejected;
    }
    assert(failures == 2 && completions == 2 && rejected == 62); // Each generation gets one failure plus 31 request errors.
    assert(!fclose(capture));
    CFRelease(surface);
}

int main(void) {
    alarm(20);
    @autoreleasepool { rawTests(); }
    CFRunLoopRef mainLoop = CFRunLoopGetCurrent();
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        @autoreleasepool {
            cancellationTests(); delayedCancellationTest(NO); delayedCancellationTest(YES); asynchronousCompletionTests();
            diagnosticTests(); dirtyRectangleTests(); stockClientTests();
        }
        dispatch_async(dispatch_get_main_queue(), ^{ CFRunLoopStop(mainLoop); });
    });
    CFRunLoopRun();
    puts("FramebufferServerTests passed (including real stock client surface and callback)");
    return 0;
}
