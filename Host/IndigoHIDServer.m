#import "IndigoHIDServer.h"
#include <errno.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { TouchCapacity = 16, KeyboardCapacity = 0xe8, IndigoDigitizer = 11, IndigoBuiltinTouch = 50, IndigoDynamicTouch = 0x40000001,
       DigitizerRange = 1, DigitizerTouch = 2, DigitizerPosition = 4, DigitizerCancel = 128 };

// SimulatorHID 1171.7's Indigo wire format: target at 76, parent index at 144, timestamp flags at 145.
// The receiver reads timestamp flags even for legacy strides; always include the modern 148-byte payload.
// These are wire payloads, not native IOHIDEvent structures. Message byte 28 counts payloads, including the parent.
#pragma pack(push, 4)
typedef struct {
    uint32_t type;
    uint64_t timestamp;
    uint32_t flags, index, identity, mask;
    double x, y, z, pressure, twist;
    uint32_t range, touch, target, collection, transducerType;
    double quality, density, irregularity, minorRadius, majorRadius;
    uint32_t other[4];
    int8_t parentIndex;
    uint8_t timestampFlags;
    uint16_t reserved;
} IndigoPayload;
typedef struct {
    mach_msg_header_t header;
    uint32_t stride;
    uint8_t count, padding[3];
    IndigoPayload payloads[TouchCapacity + 1];
} IndigoTouches;
typedef struct {
    mach_msg_header_t header;
    uint32_t stride;
    uint8_t count, padding[3];
    uint32_t type;
    uint64_t timestamp;
    uint32_t flags, command, reserved[3], target;
    uint8_t builtIn;
    char displayUID[64];
    uint8_t unused[43];
    int8_t parentIndex;
    uint8_t timestampFlags;
    uint16_t tail;
} IndigoCreateDigitizer;
typedef struct {
    mach_msg_header_t header;
    uint32_t stride;
    uint8_t count, padding[3];
    uint32_t type;
    uint64_t timestamp;
    uint32_t flags, subtype, operation, target, usage, keyboardType, usagePage;
    uint8_t unused[104];
    int8_t parentIndex;
    uint8_t timestampFlags;
    uint16_t reserved;
} IndigoKeyboard;
#pragma pack(pop)
_Static_assert(sizeof(IndigoPayload) == 148 && offsetof(IndigoPayload, x) == 28 && offsetof(IndigoPayload, transducerType) == 84 &&
               offsetof(IndigoPayload, target) == 76 && offsetof(IndigoPayload, parentIndex) == 144 &&
               offsetof(IndigoPayload, timestampFlags) == 145,
               "Indigo digitizer payload layout");
_Static_assert(offsetof(IndigoTouches, payloads) == 32, "Indigo message payload offset");
_Static_assert(sizeof(IndigoCreateDigitizer) == 32 + sizeof(IndigoPayload) && offsetof(IndigoCreateDigitizer, command) == 48 &&
               offsetof(IndigoCreateDigitizer, target) == 64 && offsetof(IndigoCreateDigitizer, builtIn) == 68 &&
               offsetof(IndigoCreateDigitizer, displayUID) == 69, "Indigo create-digitizer command layout");
_Static_assert(sizeof(IndigoKeyboard) == 32 + 148 && offsetof(IndigoKeyboard, subtype) == 48 &&
               offsetof(IndigoKeyboard, target) == 56 && offsetof(IndigoKeyboard, usage) == 60 &&
               offsetof(IndigoKeyboard, usagePage) == 68 && offsetof(IndigoKeyboard, parentIndex) == 32 + 144,
               "Indigo generic keyboard payload layout");

typedef struct { uint64_t identifier; uint32_t identity; double x, y; LSBIndigoTouchEdge edge; bool active; } Touch;

static uint32_t digitizerEdgeMask(LSBIndigoTouchEdge edge) {
    // SimulatorKit's IndigoHIDMessageForMouseNSEvent retains these origin bits on down, move, and up.
    static const uint32_t masks[] = {0, 0x02040000, 0x08040000, 0x01040000, 0x04040000};
    return masks[edge];
}

@interface LSBIndigoHIDServer () {
    dispatch_queue_t _queue;
    dispatch_source_t _source;
    mach_port_t _registration, _eventPort;
    audit_token_t _expectedAudit;
    BOOL _authorized, _closed, _digitizerReady;
    BOOL _cancelPending;
    uint64_t _cancelGeneration;
    pid_t _clientPID;
    uint32_t _nextIdentity;
    Touch _touches[TouchCapacity];
    bool _pressedKeys[KeyboardCapacity], _pendingKeyReleases[KeyboardCapacity];
    BOOL _keyReleasePending;
    uint64_t _keyReleaseGeneration;
}
- (void)drainRegistrations;
- (void)dropClient;
- (int)sendMessage:(mach_msg_header_t *)message size:(mach_msg_size_t)size;
- (int)ensureDigitizer;
- (int)sendTouches:(const Touch *)touches changed:(unsigned)changed phase:(LSBIndigoTouchPhase)phase cancellingAll:(BOOL)cancelAll;
- (void)scheduleCancellationRetry:(uint64_t)generation;
- (void)retryCancellation:(uint64_t)generation;
- (int)sendKeyboardPacket:(uint32_t)usage pressed:(BOOL)pressed;
- (int)flushKeyReleases;
- (void)scheduleKeyReleaseRetry:(uint64_t)generation;
- (void)retryKeyReleases:(uint64_t)generation;
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
    _digitizerReady = NO;
    _cancelPending = NO;
    ++_cancelGeneration;
    memset(_touches, 0, sizeof(_touches));
    memset(_pressedKeys, 0, sizeof(_pressedKeys));
    memset(_pendingKeyReleases, 0, sizeof(_pendingKeyReleases));
    _keyReleasePending = NO;
    ++_keyReleaseGeneration;
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
                int error = [self ensureDigitizer];
                if (error && error != EAGAIN) [self dropClient];
            }
        }
        mach_msg_destroy(header);
    }
}

- (int)sendMessage:(mach_msg_header_t *)message size:(mach_msg_size_t)size {
    if (_closed) return EBADF;
    if (!MACH_PORT_VALID(_eventPort)) return ENOTCONN;
    *message = (mach_msg_header_t){.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0),
        .msgh_size = size, .msgh_remote_port = _eventPort};
    mach_msg_return_t result = mach_msg(message, MACH_SEND_MSG | MACH_SEND_TIMEOUT, size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
    // A timed-out send copies its COPY_SEND destination back as a new send uref; release only that returned copy.
    if (result && MACH_MSGH_BITS_REMOTE(message->msgh_bits) == MACH_MSG_TYPE_PORT_SEND) mach_msg_destroy(message);
    if (result == MACH_SEND_INVALID_DEST) { [self dropClient]; return EPIPE; }
    return result == MACH_MSG_SUCCESS ? 0 : result == MACH_SEND_TIMED_OUT ? EAGAIN : EIO;
}

- (int)ensureDigitizer {
    if (_digitizerReady) return 0;
    // The stock receiver requires a dynamic target with bit 30 set. Built-in creation also installs its canonical alias 50.
    // PurpleMain is the UID advertised by our framebuffer. FIFO delivery guarantees creation precedes every touch.
    IndigoCreateDigitizer message = {.stride = sizeof(IndigoPayload), .count = 1, .type = 0x7fff0001,
        .timestamp = mach_absolute_time(), .command = 1, .target = IndigoDynamicTouch, .builtIn = 1,
        .displayUID = "PurpleMain", .parentIndex = -1};
    int error = [self sendMessage:&message.header size:sizeof(message)];
    if (!error) {
        _digitizerReady = YES;
        fprintf(stderr, "[indigo] built-in digitizer queued native=%d target=%u alias=%u display=PurpleMain\n",
                _clientPID, IndigoDynamicTouch, IndigoBuiltinTouch);
    }
    return error;
}

- (int)sendTouches:(const Touch *)touches changed:(unsigned)changed phase:(LSBIndigoTouchPhase)phase cancellingAll:(BOOL)cancelAll {
    if (_closed) return EBADF;
    if (!MACH_PORT_VALID(_eventPort)) return ENOTCONN;
    int error = [self ensureDigitizer];
    if (error) return error; // A full queue must never allow a touch to overtake digitizer creation.
    IndigoTouches message = {.stride = sizeof(IndigoPayload), .count = 1};
    uint64_t timestamp = mach_absolute_time();
    unsigned mask = phase == LSBIndigoTouchMoved ? DigitizerPosition : DigitizerRange | DigitizerTouch;
    if (cancelAll || phase == LSBIndigoTouchCancelled) mask |= DigitizerCancel;
    BOOL active = NO;
    unsigned parentContact = changed;
    for (unsigned i = 0; i < TouchCapacity; ++i) {
        if (!touches[i].active && i != changed) continue;
        if (parentContact == TouchCapacity) parentContact = i;
        BOOL down = touches[i].active && !cancelAll;
        active |= down;
        message.payloads[message.count++] = (IndigoPayload){.type = IndigoDigitizer, .timestamp = timestamp,
            .index = i + 1, .identity = touches[i].identity, .mask = (i == changed || cancelAll ? mask : 0) | digitizerEdgeMask(touches[i].edge),
            .x = touches[i].x, .y = touches[i].y, .pressure = down ? 1 : 0, .range = down, .touch = down,
            .target = IndigoBuiltinTouch, .transducerType = 2, .parentIndex = 0};
    }
    // Like the stock mouse builder, the parent carries this sample's position and edge. Each sibling keeps its own origin.
    const Touch *contact = parentContact < TouchCapacity ? &touches[parentContact] : NULL;
    message.payloads[0] = (IndigoPayload){.type = IndigoDigitizer, .timestamp = timestamp, .index = 0x00400002,
        .mask = mask | (contact ? digitizerEdgeMask(contact->edge) : 0), .x = contact ? contact->x : 0, .y = contact ? contact->y : 0,
        .range = active, .touch = active, .target = IndigoBuiltinTouch, .collection = 1, .transducerType = 3, .parentIndex = -1};
    return [self sendMessage:&message.header size:(mach_msg_size_t)(offsetof(IndigoTouches, payloads) + message.count * sizeof(IndigoPayload))];
}

- (int)sendTouch:(uint64_t)identifier phase:(LSBIndigoTouchPhase)phase x:(double)x y:(double)y {
    return [self sendTouch:identifier phase:phase x:x y:y edge:LSBIndigoTouchEdgeNone];
}

- (int)sendTouch:(uint64_t)identifier phase:(LSBIndigoTouchPhase)phase x:(double)x y:(double)y edge:(LSBIndigoTouchEdge)edge {
    if (phase > LSBIndigoTouchCancelled || (unsigned)edge > LSBIndigoTouchEdgeRight || !isfinite(x) || !isfinite(y) ||
        (phase != LSBIndigoTouchBegan && edge != LSBIndigoTouchEdgeNone)) return EINVAL;
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
            next[found] = (Touch){.identifier = identifier, .identity = identity, .edge = edge, .active = true};
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

- (int)sendKeyboardPacket:(uint32_t)usage pressed:(BOOL)pressed {
    // SimulatorKit 1171.7 IndigoHIDMessageForHIDArbitrary(target, page, usage, operation), decoded by SimulatorHID +0x67b4.
    // Its generic subtype bypasses _IndigoHIDUpdateKeyboardType: typing must not toggle the host's keyboard attachment state.
    IndigoKeyboard message = {.stride = 148, .count = 1, .type = 2, .timestamp = mach_absolute_time(),
        .subtype = 10001, .operation = pressed ? 1 : 2, .target = 100, .usage = usage, .usagePage = 7, .parentIndex = -1};
    return [self sendMessage:&message.header size:sizeof(message)];
}

- (int)sendKeyboardUsage:(uint32_t)usage pressed:(BOOL)pressed {
    if (usage < 4 || usage >= KeyboardCapacity) return EINVAL;
    __block int result = 0;
    [self serialized:^{
        if (self->_closed) { result = EBADF; return; }
        if (!MACH_PORT_VALID(self->_eventPort)) { result = ENOTCONN; return; }
        if (self->_keyReleasePending) { result = EBUSY; return; }
        if (self->_pressedKeys[usage] == !!pressed) { result = pressed ? EALREADY : ENOENT; return; }
        result = [self sendKeyboardPacket:usage pressed:pressed];
        if (!result) self->_pressedKeys[usage] = !!pressed;
        else if (!pressed && result == EAGAIN) {
            self->_pendingKeyReleases[usage] = true;
            self->_keyReleasePending = YES;
            [self scheduleKeyReleaseRetry:++self->_keyReleaseGeneration];
        }
    }];
    return result;
}

- (int)flushKeyReleases {
    for (unsigned usage = 4; usage < KeyboardCapacity; ++usage) {
        if (!_pendingKeyReleases[usage]) continue;
        int result = [self sendKeyboardPacket:usage pressed:NO];
        if (result) return result;
        _pressedKeys[usage] = false;
        _pendingKeyReleases[usage] = false;
    }
    _keyReleasePending = NO;
    return 0;
}

- (int)releaseAllKeys {
    __block int result = 0;
    [self serialized:^{
        BOOL active = NO;
        for (unsigned usage = 4; usage < KeyboardCapacity; ++usage) {
            self->_pendingKeyReleases[usage] |= self->_pressedKeys[usage];
            active |= self->_pendingKeyReleases[usage];
        }
        if (!active) return;
        if (self->_keyReleasePending) { result = EAGAIN; return; }
        result = [self flushKeyReleases];
        if (result == EAGAIN) {
            self->_keyReleasePending = YES;
            [self scheduleKeyReleaseRetry:++self->_keyReleaseGeneration];
        } else if (result) [self dropClient];
    }];
    return result;
}

- (void)scheduleKeyReleaseRetry:(uint64_t)generation {
    __weak LSBIndigoHIDServer *weakSelf = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC), _queue, ^{ [weakSelf retryKeyReleases:generation]; });
}

- (void)retryKeyReleases:(uint64_t)generation {
    if (_closed || !_keyReleasePending || generation != _keyReleaseGeneration) return;
    int error = [self flushKeyReleases];
    if (error == EAGAIN) [self scheduleKeyReleaseRetry:generation];
    else if (error) [self dropClient];
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
