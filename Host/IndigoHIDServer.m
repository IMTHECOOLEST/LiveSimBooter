#import "IndigoHIDServer.h"
#include <errno.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { TouchCapacity = 16, IndigoDigitizer = 11, DigitizerRange = 1, DigitizerTouch = 2, DigitizerPosition = 4, DigitizerCancel = 128 };

// Indigo's packed payload has a 144-byte stride. Offset 28 in the message is the number of payloads, including the parent.
// Verified against SimulatorHID's _IOHIDEventFromIndigoHIDData and its Mach receive loop, not native IOHIDEvent memory layout.
#pragma pack(push, 4)
typedef struct {
    uint32_t type;
    uint64_t timestamp;
    uint32_t flags, index, identity, mask;
    double x, y, z, pressure, twist;
    uint32_t range, touch, options, collection, transducerType;
    double quality, density, irregularity, minorRadius, majorRadius;
    uint32_t other[4];
} IndigoPayload;
typedef struct {
    mach_msg_header_t header;
    uint32_t stride;
    uint8_t count, padding[3];
    IndigoPayload payloads[TouchCapacity + 1];
} IndigoTouches;
#pragma pack(pop)
_Static_assert(sizeof(IndigoPayload) == 144 && offsetof(IndigoPayload, x) == 28 && offsetof(IndigoPayload, transducerType) == 84,
               "Indigo digitizer payload layout");
_Static_assert(offsetof(IndigoTouches, payloads) == 32, "Indigo message payload offset");

typedef struct { uint64_t identifier; uint32_t identity; double x, y; bool active; } Touch;

@interface LSBIndigoHIDServer () {
    dispatch_queue_t _queue;
    dispatch_source_t _source;
    mach_port_t _registration, _eventPort;
    audit_token_t _expectedAudit;
    BOOL _authorized, _closed;
    BOOL _cancelPending;
    uint64_t _cancelGeneration;
    pid_t _clientPID;
    uint32_t _nextIdentity;
    Touch _touches[TouchCapacity];
}
- (void)drainRegistrations;
- (void)dropClient;
- (int)sendTouches:(const Touch *)touches changed:(unsigned)changed phase:(LSBIndigoTouchPhase)phase cancellingAll:(BOOL)cancelAll;
- (void)scheduleCancellationRetry:(uint64_t)generation;
- (void)retryCancellation:(uint64_t)generation;
@end

@implementation LSBIndigoHIDServer
- (instancetype)initWithError:(int *)error {
    if (error) *error = 0;
    if (!(self = [super init])) { if (error) *error = ENOMEM; return nil; }
    _queue = dispatch_queue_create("com.kdt.LiveSimBooter.indigo", DISPATCH_QUEUE_SERIAL);
    dispatch_queue_set_specific(_queue, (__bridge const void *)self, (__bridge void *)self, NULL);
    kern_return_t result = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &_registration);
    if (!result) result = mach_port_insert_right(mach_task_self(), _registration, _registration, MACH_MSG_TYPE_MAKE_SEND);
    if (result) {
        if (MACH_PORT_VALID(_registration)) mach_port_mod_refs(mach_task_self(), _registration, MACH_PORT_RIGHT_RECEIVE, -1);
        _registration = MACH_PORT_NULL;
        if (error) *error = result == KERN_RESOURCE_SHORTAGE ? ENOMEM : EIO;
        return nil;
    }
    _source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, _registration, 0, _queue);
    if (!_source) {
        mach_port_mod_refs(mach_task_self(), _registration, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), _registration);
        _registration = MACH_PORT_NULL;
        if (error) *error = ENOMEM;
        return nil;
    }
    mach_port_t port = _registration;
    dispatch_source_set_cancel_handler(_source, ^{
        mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), port);
    });
    __weak LSBIndigoHIDServer *weakSelf = self;
    dispatch_source_set_event_handler(_source, ^{ [weakSelf drainRegistrations]; });
    dispatch_resume(_source);
    return self;
}

- (void)serialized:(dispatch_block_t)block {
    if (dispatch_get_specific((__bridge const void *)self)) block();
    else dispatch_sync(_queue, block);
}

- (mach_port_t)registrationPort {
    __block mach_port_t port;
    [self serialized:^{ port = self->_closed ? MACH_PORT_NULL : self->_registration; }];
    return port;
}

- (pid_t)connectedClientPID {
    __block pid_t pid;
    [self serialized:^{ pid = self->_clientPID; }];
    return pid;
}

- (void)dropClient {
    if (MACH_PORT_VALID(_eventPort)) mach_port_deallocate(mach_task_self(), _eventPort);
    _eventPort = MACH_PORT_NULL;
    _clientPID = 0;
    _cancelPending = NO;
    ++_cancelGeneration;
    memset(_touches, 0, sizeof(_touches));
}

- (void)authorizeClientWithAuditToken:(const audit_token_t *)audit {
    audit_token_t token = {{0}};
    if (audit) token = *audit;
    [self serialized:^{
        if (self->_closed) return;
        if (self->_authorized && !memcmp(&token, &self->_expectedAudit, sizeof(token))) return;
        [self dropClient];
        self->_expectedAudit = token;
        self->_authorized = token.val[5] > 0 && token.val[5] <= INT_MAX;
    }];
}

- (void)drainRegistrations {
    if (_closed) return;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        union { mach_msg_header_t header; uint8_t bytes[1024 + MAX_TRAILER_SIZE]; } message = {0};
        mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                                    MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
        // No MACH_RCV_LARGE: oversized requests are discarded rather than blocking subsequent registrations.
        mach_msg_return_t result = mach_msg(&message.header, options, 0, sizeof(message), _registration, 0, MACH_PORT_NULL);
        if (result == MACH_RCV_TIMED_OUT) break;
        if (result) {
            if ((result & ~MACH_MSG_MASK) == MACH_RCV_BODY_ERROR) mach_msg_destroy(&message.header);
            if (result == MACH_RCV_TOO_LARGE || result == MACH_RCV_INTERRUPTED) continue;
            break;
        }
        mach_msg_header_t *header = &message.header;
        if (_authorized && header->msgh_size == sizeof(*header) && header->msgh_id == 0 && header->msgh_local_port == _registration &&
            header->msgh_bits == MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND, MACH_MSG_TYPE_PORT_SEND) &&
            header->msgh_voucher_port == MACH_PORT_NULL && MACH_PORT_VALID(header->msgh_remote_port)) {
            mach_msg_audit_trailer_t trailer;
            memcpy(&trailer, message.bytes + sizeof(*header), sizeof(trailer));
            if (trailer.msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0 && trailer.msgh_trailer_size >= sizeof(trailer) &&
                !memcmp(&trailer.msgh_audit, &_expectedAudit, sizeof(_expectedAudit))) {
                [self dropClient];
                _eventPort = header->msgh_remote_port;
                header->msgh_remote_port = MACH_PORT_NULL;
                _clientPID = (pid_t)trailer.msgh_audit.val[5];
                fprintf(stderr, "[indigo] guest event port registered native=%d\n", _clientPID);
            }
        }
        mach_msg_destroy(header);
    }
}

- (int)sendTouches:(const Touch *)touches changed:(unsigned)changed phase:(LSBIndigoTouchPhase)phase cancellingAll:(BOOL)cancelAll {
    if (_closed) return EBADF;
    if (!MACH_PORT_VALID(_eventPort)) return ENOTCONN;
    IndigoTouches message = {.stride = sizeof(IndigoPayload), .count = 1};
    uint64_t timestamp = mach_absolute_time();
    unsigned mask = phase == LSBIndigoTouchMoved ? DigitizerPosition : DigitizerRange | DigitizerTouch;
    if (cancelAll || phase == LSBIndigoTouchCancelled) mask |= DigitizerCancel;
    BOOL active = NO;
    for (unsigned i = 0; i < TouchCapacity; ++i) {
        if (!touches[i].active && i != changed) continue;
        BOOL down = touches[i].active && !cancelAll;
        active |= down;
        message.payloads[message.count++] = (IndigoPayload){.type = IndigoDigitizer, .timestamp = timestamp,
            .index = i + 1, .identity = touches[i].identity, .mask = i == changed || cancelAll ? mask : 0,
            .x = touches[i].x, .y = touches[i].y, .pressure = down ? 1 : 0, .range = down, .touch = down, .transducerType = 2};
    }
    message.payloads[0] = (IndigoPayload){.type = IndigoDigitizer, .timestamp = timestamp, .index = 0x00400002,
        .mask = mask, .range = active, .touch = active, .collection = 1, .transducerType = 3};
    message.header = (mach_msg_header_t){.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0),
        .msgh_size = (mach_msg_size_t)(offsetof(IndigoTouches, payloads) + message.count * sizeof(IndigoPayload)), .msgh_remote_port = _eventPort};
    mach_msg_return_t result = mach_msg(&message.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, message.header.msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
    // A timed-out send copies its COPY_SEND destination back as a new send uref; release only that returned copy.
    if (result && MACH_MSGH_BITS_REMOTE(message.header.msgh_bits) == MACH_MSG_TYPE_PORT_SEND) mach_msg_destroy(&message.header);
    if (result == MACH_SEND_INVALID_DEST) { [self dropClient]; return EPIPE; }
    return result == MACH_MSG_SUCCESS ? 0 : result == MACH_SEND_TIMED_OUT ? EAGAIN : EIO;
}

- (int)sendTouch:(uint64_t)identifier phase:(LSBIndigoTouchPhase)phase x:(double)x y:(double)y {
    if (phase > LSBIndigoTouchCancelled || !isfinite(x) || !isfinite(y)) return EINVAL;
    __block int result = 0;
    [self serialized:^{
        if (self->_closed) { result = EBADF; return; }
        if (self->_cancelPending) { result = EBUSY; return; }
        Touch next[TouchCapacity];
        memcpy(next, self->_touches, sizeof(next));
        unsigned found = TouchCapacity, freeSlot = TouchCapacity;
        for (unsigned i = 0; i < TouchCapacity; ++i) {
            if (next[i].active && next[i].identifier == identifier) found = i;
            if (!next[i].active && freeSlot == TouchCapacity) freeSlot = i;
        }
        if (phase == LSBIndigoTouchBegan) {
            if (found != TouchCapacity) { result = EALREADY; return; }
            if (freeSlot == TouchCapacity) { result = ENOSPC; return; }
            found = freeSlot;
            uint32_t identity = self->_nextIdentity;
            BOOL collision;
            do {
                if (!++identity) ++identity;
                collision = NO;
                for (unsigned i = 0; i < TouchCapacity; ++i) if (next[i].active && next[i].identity == identity) collision = YES;
            } while (collision);
            next[found] = (Touch){.identifier = identifier, .identity = identity, .active = true};
        } else if (found == TouchCapacity) { result = ENOENT; return; }
        next[found].x = fmin(1, fmax(0, x));
        next[found].y = fmin(1, fmax(0, y));
        if (phase == LSBIndigoTouchEnded || phase == LSBIndigoTouchCancelled) next[found].active = false;
        result = [self sendTouches:next changed:found phase:phase cancellingAll:NO];
        if (!result) {
            memcpy(self->_touches, next, sizeof(next));
            if (phase == LSBIndigoTouchBegan) self->_nextIdentity = next[found].identity;
        }
    }];
    return result;
}

- (int)cancelTouches {
    __block int result = 0;
    [self serialized:^{
        if (self->_cancelPending) { result = EAGAIN; return; }
        BOOL active = NO;
        for (unsigned i = 0; i < TouchCapacity; ++i) active |= self->_touches[i].active;
        if (!active) return;
        result = [self sendTouches:self->_touches changed:TouchCapacity phase:LSBIndigoTouchCancelled cancellingAll:YES];
        if (!result) memset(self->_touches, 0, sizeof(self->_touches));
        else if (result == EAGAIN) {
            self->_cancelPending = YES;
            [self scheduleCancellationRetry:++self->_cancelGeneration];
        }
    }];
    return result;
}

- (void)scheduleCancellationRetry:(uint64_t)generation {
    __weak LSBIndigoHIDServer *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC), _queue, ^{ [weakSelf retryCancellation:generation]; });
}

- (void)retryCancellation:(uint64_t)generation {
    if (_closed || !_cancelPending || generation != _cancelGeneration) return;
    int error = [self sendTouches:_touches changed:TouchCapacity phase:LSBIndigoTouchCancelled cancellingAll:YES];
    if (error == EAGAIN) [self scheduleCancellationRetry:generation];
    else if (error) [self dropClient];
    else {
        memset(_touches, 0, sizeof(_touches));
        _cancelPending = NO;
    }
}

- (void)close {
    if (!_queue) return;
    __unsafe_unretained LSBIndigoHIDServer *server = self;
    [self serialized:^{
        if (server->_closed) return;
        server->_closed = YES;
        server->_authorized = NO;
        [server dropClient];
        if (server->_source) dispatch_source_cancel(server->_source);
        server->_source = nil;
        server->_registration = MACH_PORT_NULL;
    }];
}

- (void)dealloc { [self close]; }
@end
