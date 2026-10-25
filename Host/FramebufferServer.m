#import "FramebufferServer.h"
#include "FramebufferProtocol.h"
#include <errno.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { FBItemSize = sizeof(FBWireItem), FBMaxItems = 32, FBMaxDescriptors = 64, FBMaxMessage = 16384, FBWidth = 1290, FBHeight = 2796 };
static const uint64_t FBMagic = UINT64_C(0x00ace0ace0face01), FBItemMagic = UINT64_C(0xfadecafe00007777);
static uint64_t monotonicNanoseconds(void) {
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return (uint64_t)((__uint128_t)mach_absolute_time() * timebase.numer / timebase.denom);
}

typedef struct {
    mach_msg_header_t *header;
    mach_msg_port_descriptor_t *descriptors;
    uint32_t descriptorCount, itemCount;
    uint8_t *session, *items;
} FBRequest;
typedef struct {
    FBWireMessagePrefix prefix;
    FBWireEnvelope envelope;
    FBWireItem items[4];
    uint8_t trailingPadding[FBWireTrailingPadding];
} FBReply;
_Static_assert(offsetof(FBReply, envelope) == 28 && offsetof(FBReply, items) == 56 &&
               sizeof(FBReply) == FBWireEmptyMessageSize + 4 * FBItemSize, "framebuffer reply ABI");

// The stock serializer includes 20 zero bytes after its last fixed-size item. Never confuse these with the audit trailer.
static BOOL parseRequest(mach_msg_header_t *header, FBRequest *request) {
    if (header->msgh_size < FBWireEmptyMessageSize || header->msgh_size > FBMaxMessage || !(header->msgh_bits & MACH_MSGH_BITS_COMPLEX) ||
        header->msgh_voucher_port || MACH_MSGH_BITS_LOCAL(header->msgh_bits) != MACH_MSG_TYPE_PORT_SEND ||
        (MACH_MSGH_BITS_REMOTE(header->msgh_bits) != MACH_MSG_TYPE_PORT_SEND_ONCE && MACH_MSGH_BITS_REMOTE(header->msgh_bits) != 0)) return NO;
    uint8_t *bytes = (void *)header;
    FBWireMessagePrefix prefix;
    memcpy(&prefix, bytes, sizeof(prefix));
    uint32_t count = prefix.body.msgh_descriptor_count;
    if (count > FBMaxDescriptors) return NO;
    size_t descriptorBytes = sizeof(mach_msg_port_descriptor_t) * count + sizeof(uint32_t) * (count & 1);
    if (header->msgh_size < FBWireEmptyMessageSize + descriptorBytes) return NO;
    size_t envelopeOffset = sizeof(prefix) + descriptorBytes;
    FBWireEnvelope envelope;
    memcpy(&envelope, bytes + envelopeOffset, sizeof(envelope));
    if (envelope.magic != FBMagic || envelope.itemCount > FBMaxItems ||
        header->msgh_size != FBWireEmptyMessageSize + descriptorBytes + FBItemSize * envelope.itemCount) return NO;
    mach_msg_port_descriptor_t *descriptors = (void *)(bytes + sizeof(prefix));
    for (uint32_t i = 0; i < count; ++i) {
        if (descriptors[i].type != MACH_MSG_PORT_DESCRIPTOR || !MACH_PORT_VALID(descriptors[i].name) ||
            (descriptors[i].disposition != MACH_MSG_TYPE_PORT_SEND && descriptors[i].disposition != MACH_MSG_TYPE_PORT_RECEIVE)) return NO;
    }
    uint8_t *first = bytes + envelopeOffset + sizeof(envelope);
    for (uint32_t i = 0; i < envelope.itemCount; ++i) {
        FBWireItemHeader item;
        memcpy(&item, first + FBItemSize * i, sizeof(item));
        if (item.magic != FBItemMagic || item.stride != FBItemSize || !item.type) return NO;
    }
    *request = (FBRequest){header, descriptors, count, envelope.itemCount, bytes + envelopeOffset + offsetof(FBWireEnvelope, session), first};
    return YES;
}

// parseRequest has validated every complete item before either helper is used. Keep typed payloads local to avoid unaligned wire reads.
static uint32_t firstItemType(const FBRequest *request) {
    if (!request->itemCount) return 0;
    FBWireItemHeader item;
    memcpy(&item, request->items, sizeof(item));
    return item.type;
}
static BOOL copyPayload(const FBRequest *request, uint32_t type, void *payload, size_t size) {
    if (request->itemCount != 1 || firstItemType(request) != type || size > sizeof(((FBWireItem *)0)->payload)) return NO;
    memcpy(payload, request->items + offsetof(FBWireItem, payload), size);
    return YES;
}

static void beginReply(FBReply *reply, mach_msg_id_t identifier, const uint8_t *session) {
    memset(reply, 0, sizeof(*reply));
    mach_msg_header_t *header = &reply->prefix.header;
    header->msgh_size = FBWireEmptyMessageSize;
    header->msgh_id = identifier;
    reply->envelope.magic = FBMagic;
    if (session) memcpy(reply->envelope.session, session, sizeof(reply->envelope.session));
}
static BOOL addItem(FBReply *reply, uint32_t type, const void *payload, size_t size) {
    uint32_t count = reply->envelope.itemCount;
    if (count >= sizeof(reply->items) / sizeof(reply->items[0]) || size > sizeof(reply->items[0].payload)) return NO;
    FBWireItem item = {.header = {.magic = FBItemMagic, .type = type, .stride = FBItemSize}};
    memcpy(item.payload, payload, size);
    memcpy(&reply->items[count], &item, sizeof(item));
    reply->envelope.itemCount = count + 1;
    reply->prefix.header.msgh_size += FBItemSize;
    return YES;
}
static void addError(FBReply *reply, int error) {
    FBWireError payload = {.domain = 1001, .code = error};
    snprintf(payload.description, sizeof(payload.description), "%s", strerror(error));
    addItem(reply, 17, &payload, sizeof(payload));
}
static int sendReply(FBReply *reply, mach_port_t destination, BOOL once) {
    if (!MACH_PORT_VALID(destination)) return EINVAL;
    mach_msg_header_t *header = &reply->prefix.header;
    header->msgh_remote_port = destination;
    header->msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(once ? MACH_MSG_TYPE_MOVE_SEND_ONCE : MACH_MSG_TYPE_COPY_SEND, 0);
    mach_msg_return_t result = mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, header->msgh_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
    if (result && (once || MACH_MSGH_BITS_REMOTE(header->msgh_bits) == MACH_MSG_TYPE_PORT_SEND)) mach_msg_destroy(header);
    return !result ? 0 : result == MACH_SEND_TIMED_OUT ? EAGAIN : EPIPE;
}

@interface FBPresentation : NSObject {
@public
    IOSurfaceRef surface;
    mach_port_t fence, replyPort;
    uint64_t generation, operation, desired, completed;
    uint32_t chain, surfaceID;
    pid_t nativePID;
    CGRect dirtyRect;
    BOOL callback, fullFrame;
    dispatch_source_t timer;
    _Atomic(bool) cancelled;
    _Atomic(unsigned) stage; // 0 queued, 1 handler owns source reads, 2 completed/cancelled.
}
@end
@implementation FBPresentation
- (void)dealloc {
    if (timer) dispatch_source_cancel(timer);
    if (surface) CFRelease(surface);
    if (MACH_PORT_VALID(fence)) mach_port_mod_refs(mach_task_self(), fence, MACH_PORT_RIGHT_RECEIVE, -1);
    if (MACH_PORT_VALID(replyPort)) mach_port_deallocate(mach_task_self(), replyPort);
}
@end

@interface LSBFramebufferServer () {
    dispatch_queue_t _queue;
    dispatch_source_t _source, _eventDeath;
    mach_port_t _receive, _events;
    _Atomic(mach_port_t) _publishedPort;
    _Atomic(pid_t) _publishedPID;
    _Atomic(uint64_t) _publishedGeneration;
    _Atomic(uint64_t) _generation;
    _Atomic(bool) _closing;
    uint64_t _activeGeneration;
    audit_token_t _audit;
    BOOL _authorized, _connected, _hasBaseline;
    FBPresentation *_pending;
    FBPresentation *_mainWork; // One queued main block or asynchronous copy, retained until the block drains or copy completes.
    uint8_t _session[16];
    uint32_t _swapchain, _nextSwapchain, _presentationMode;
    uint64_t _lastOperation;
    unsigned _diagnostics, _frames, _partialFrames;
    LSBFramebufferPresent _frameHandler;
}
- (void)drain;
- (void)dropClient;
- (void)handle:(FBRequest *)request;
- (int)present:(FBRequest *)request;
- (void)enqueuePresentation:(FBPresentation *)presentation handler:(LSBFramebufferPresent)handler;
- (void)finish:(FBPresentation *)presentation error:(int)error;
@end

@implementation LSBFramebufferServer
@synthesize frameHandler = _frameHandler;

- (instancetype)initWithError:(int *)error {
    if (error) *error = 0;
    if (!(self = [super init])) { if (error) *error = ENOMEM; return nil; }
    _queue = dispatch_queue_create("com.kdt.LiveSimBooter.framebuffer", DISPATCH_QUEUE_SERIAL);
    kern_return_t result = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &_receive);
    if (!result) result = mach_port_insert_right(mach_task_self(), _receive, _receive, MACH_MSG_TYPE_MAKE_SEND);
    if (result) {
        if (MACH_PORT_VALID(_receive)) mach_port_mod_refs(mach_task_self(), _receive, MACH_PORT_RIGHT_RECEIVE, -1);
        _receive = MACH_PORT_NULL;
        if (error) *error = result == KERN_RESOURCE_SHORTAGE ? ENOMEM : EIO;
        return nil;
    }
    _source = dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_RECV, _receive, 0, _queue);
    if (!_source) {
        mach_port_mod_refs(mach_task_self(), _receive, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), _receive);
        _receive = MACH_PORT_NULL;
        if (error) *error = ENOMEM;
        return nil;
    }
    mach_port_t port = _receive;
    dispatch_source_set_cancel_handler(_source, ^{
        mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
        mach_port_deallocate(mach_task_self(), port);
    });
    __weak LSBFramebufferServer *weakSelf = self;
    dispatch_source_set_event_handler(_source, ^{ [weakSelf drain]; });
    atomic_store(&_publishedPort, _receive);
    dispatch_resume(_source);
    return self;
}
- (mach_port_t)registrationPort { return atomic_load(&_publishedPort); }
- (pid_t)connectedClientPID {
    return atomic_load(&_publishedGeneration) == atomic_load(&_generation) ? atomic_load(&_publishedPID) : 0;
}

- (void)dropClient {
    if (_eventDeath) { dispatch_source_cancel(_eventDeath); _eventDeath = nil; }
    if (MACH_PORT_VALID(_events)) mach_port_deallocate(mach_task_self(), _events);
    _events = MACH_PORT_NULL;
    _connected = NO;
    if (_pending) {
        unsigned queued = 0;
        if (atomic_compare_exchange_strong(&_pending->stage, &queued, 2)) [self finish:_pending error:ECANCELED];
        _pending = nil;
    }
    _swapchain = 0;
    _presentationMode = 0;
    _hasBaseline = NO;
    _lastOperation = 0;
    memset(_session, 0, sizeof(_session));
    atomic_store(&_publishedPID, 0);
}
- (void)authorizeClientWithAuditToken:(const audit_token_t *)audit {
    audit_token_t copy = {{0}};
    if (audit) copy = *audit;
    uint64_t generation = atomic_fetch_add(&_generation, 1) + 1;
    atomic_store(&_publishedPID, 0);
    dispatch_async(_queue, ^{
        if (atomic_load(&self->_closing) || generation != atomic_load(&self->_generation)) return;
        [self dropClient];
        self->_frames = self->_partialFrames = self->_diagnostics = 0;
        self->_audit = copy;
        self->_activeGeneration = generation;
        self->_authorized = copy.val[5] > 0 && copy.val[5] <= INT_MAX;
    });
}
- (void)close {
    if (atomic_exchange(&_closing, true)) return;
    atomic_fetch_add(&_generation, 1);
    atomic_store(&_publishedPID, 0);
    atomic_store(&_publishedPort, MACH_PORT_NULL);
    dispatch_async(_queue, ^{
        self->_authorized = NO;
        [self dropClient];
        if (self->_source) { dispatch_source_cancel(self->_source); self->_source = nil; }
    });
}
- (void)dealloc {
    if (_source) dispatch_source_cancel(_source);
    if (_eventDeath) dispatch_source_cancel(_eventDeath);
    if (MACH_PORT_VALID(_events)) mach_port_deallocate(mach_task_self(), _events);
}

- (void)drain {
    if (atomic_load(&_closing)) return;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        if (_activeGeneration != atomic_load(&_generation)) break; // Let a queued authorization update run before receiving its client's message.
        union { mach_msg_header_t header; uint8_t bytes[FBMaxMessage + MAX_TRAILER_SIZE]; } message = {0};
        mach_msg_option_t options = MACH_RCV_MSG | MACH_RCV_TIMEOUT | MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
                                    MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT);
        mach_msg_return_t result = mach_msg(&message.header, options, 0, sizeof(message), _receive, 0, MACH_PORT_NULL);
        if (result == MACH_RCV_TIMED_OUT) break;
        if (result) {
            if ((result & ~MACH_MSG_MASK) == MACH_RCV_BODY_ERROR) mach_msg_destroy(&message.header);
            if (result == MACH_RCV_TOO_LARGE || result == MACH_RCV_INTERRUPTED) continue;
            break;
        }
        mach_msg_header_t *header = &message.header;
        FBRequest request;
        int error = EINVAL;
        size_t trailerOffset = (header->msgh_size + 3u) & ~3u;
        if (trailerOffset <= sizeof(message) - sizeof(mach_msg_audit_trailer_t) && parseRequest(header, &request)) {
            mach_msg_audit_trailer_t trailer;
            memcpy(&trailer, message.bytes + trailerOffset, sizeof(trailer));
            if (!atomic_load(&_closing) && _authorized && _activeGeneration == atomic_load(&_generation) && header->msgh_local_port == _receive &&
                trailer.msgh_trailer_type == MACH_MSG_TRAILER_FORMAT_0 && trailer.msgh_trailer_size >= sizeof(trailer) &&
                !memcmp(&trailer.msgh_audit, &_audit, sizeof(_audit))) {
                [self handle:&request];
                error = 0;
            } else error = EPERM;
        }
        if (error && MACH_MSGH_BITS_REMOTE(header->msgh_bits) == MACH_MSG_TYPE_PORT_SEND_ONCE && MACH_PORT_VALID(header->msgh_remote_port)) {
            FBReply reply;
            beginReply(&reply, header->msgh_id, NULL);
            addError(&reply, error);
            mach_port_t port = header->msgh_remote_port;
            header->msgh_remote_port = MACH_PORT_NULL;
            sendReply(&reply, port, YES);
        }
        mach_msg_destroy(header);
    }
}

- (void)handle:(FBRequest *)request {
    mach_msg_header_t *header = request->header;
    FBReply reply;
    beginReply(&reply, header->msgh_id, _connected ? _session : NULL);
    int error = 0;
    uint32_t type = firstItemType(request);
    if (header->msgh_id == 1) {
        static const uint8_t zeroSession[16] = {0};
        FBWireCheckIn checkIn;
        if (_connected) error = EALREADY;
        else if (request->descriptorCount != 1 || !copyPayload(request, 14, &checkIn, sizeof(checkIn)) || checkIn.eventDescriptorIndex != 0 ||
                 checkIn.reserved || !memchr(checkIn.name, 0, sizeof(checkIn.name)) || memcmp(request->session, zeroSession, sizeof(zeroSession)) ||
                 request->descriptors[0].disposition != MACH_MSG_TYPE_PORT_SEND || !MACH_PORT_VALID(header->msgh_remote_port)) error = EINVAL;
        else {
            mach_port_t event = request->descriptors[0].name;
            kern_return_t retained = mach_port_mod_refs(mach_task_self(), event, MACH_PORT_RIGHT_SEND, 1);
            dispatch_source_t death = retained ? nil : dispatch_source_create(DISPATCH_SOURCE_TYPE_MACH_SEND, event, DISPATCH_MACH_SEND_DEAD, _queue);
            if (!death) {
                if (!retained) mach_port_deallocate(mach_task_self(), event);
                addError(&reply, ENOMEM);
                mach_port_t port = header->msgh_remote_port;
                header->msgh_remote_port = MACH_PORT_NULL;
                sendReply(&reply, port, YES);
                return;
            }
            dispatch_source_set_cancel_handler(death, ^{ mach_port_deallocate(mach_task_self(), event); });
            uint64_t generation = _activeGeneration; // Stamp the identity which admitted this request, never a concurrently advanced authorization.
            __weak LSBFramebufferServer *weakSelf = self;
            dispatch_source_set_event_handler(death, ^{
                LSBFramebufferServer *server = weakSelf;
                uint64_t expected = generation;
                if (server && atomic_compare_exchange_strong(&server->_generation, &expected, generation + 1)) {
                    server->_activeGeneration = generation + 1;
                    server->_authorized = NO;
                    [server dropClient];
                }
            });
            dispatch_resume(death);
            arc4random_buf(_session, sizeof(_session));
            FBWireSession session;
            memcpy(session.identifier, _session, sizeof(session.identifier));
            addItem(&reply, 18, &session, sizeof(session));
            mach_port_t port = header->msgh_remote_port;
            header->msgh_remote_port = MACH_PORT_NULL;
            if (!sendReply(&reply, port, YES)) {
                _events = request->descriptors[0].name;
                request->descriptors[0].name = MACH_PORT_NULL;
                _connected = YES;
                atomic_store(&_publishedGeneration, generation);
                atomic_store(&_publishedPID, (pid_t)_audit.val[5]);
                _eventDeath = death;
                fprintf(stderr, "[framebuffer] client registered native=%d name=%s\n", _audit.val[5], checkIn.name);
            } else { dispatch_source_cancel(death); memset(_session, 0, sizeof(_session)); }
            return;
        }
    } else if (!_connected || memcmp(request->session, _session, 16)) error = ENOTCONN;
    else if (header->msgh_id == 2) {
        if (request->itemCount || request->descriptorCount) error = EINVAL;
        else {
            FBWireDisplayProperties properties = {
                .displayUID = "PurpleMain", .displayName = "LCD", .deviceName = "primary", .displayFlags = 1, // Powered on.
                .supportedPresentationModes = 3, // Immediate (1) and bounded delayed (2) presentation.
                .deviceWidth = FBWidth, .deviceHeight = FBHeight, .canvasWidth = FBWidth, .canvasHeight = FBHeight,
                .displayID = 1, .displayType = 0, .dotPitch = 460, .maximumLayerCount = 1, .maximumSwapchainCount = 1
            };
            addItem(&reply, 4, &properties, sizeof(properties));
            FBWireDisplayMode mode = {
                .modeID = 1, .width = FBWidth, .height = FBHeight, .scale = 3, .hdr = 1, .pixelFormat = 0x42475241, .colorSpace = 1,
                .refreshHundredths = 6000, .preferred = 1, .native = 1, .current = 1
            };
            addItem(&reply, 5, &mode, sizeof(mode));
        }
    } else if (header->msgh_id == 9) {
        FBWireSwapchain chain;
        if (request->descriptorCount || !copyPayload(request, 7, &chain, sizeof(chain))) error = EINVAL;
        else if (_pending) error = EAGAIN;
        else if (_swapchain) error = ENOSPC;
        else if (chain.width != FBWidth || chain.height != FBHeight || chain.displayID != 1 || chain.presentationMode > 2) error = ENOTSUP;
        else {
            if (!++_nextSwapchain) ++_nextSwapchain;
            _swapchain = _nextSwapchain;
            _hasBaseline = NO;
            _presentationMode = chain.presentationMode ? chain.presentationMode : 1;
            chain.swapchainID = _swapchain;
            chain.pixelFormat = 0x42475241;
            chain.colorSpace = 1;
            chain.hdr = 1;
            chain.presentationMode = _presentationMode;
            addItem(&reply, 7, &chain, sizeof(chain));
            fprintf(stderr, "[framebuffer] swapchain=%u size=%ux%u mode=%u\n", _swapchain, FBWidth, FBHeight, chain.presentationMode);
        }
    } else if (header->msgh_id == 11) {
        error = [self present:request];
        if (!error) return; // Reply and callback follow the asynchronous host copy's completion.
    } else if (header->msgh_id == 13 || header->msgh_id == 18) {
        BOOL destroy = header->msgh_id == 13;
        FBWireSwapchain chain = {0};
        FBWireCancel cancel = {0};
        BOOL valid = destroy ? copyPayload(request, 7, &chain, sizeof(chain)) : copyPayload(request, 22, &cancel, sizeof(cancel));
        uint32_t displayID = destroy ? chain.displayID : cancel.displayID, swapchainID = destroy ? chain.swapchainID : cancel.swapchainID;
        if (request->descriptorCount || !valid) error = EINVAL;
        else if (!_swapchain || displayID != 1 || swapchainID != _swapchain) error = ENOENT;
        else {
            if (_pending) {
                atomic_store(&_pending->cancelled, true);
                unsigned queued = 0;
                if (atomic_compare_exchange_strong(&_pending->stage, &queued, 2)) [self finish:_pending error:ECANCELED];
            }
            if (destroy) { _swapchain = 0; _presentationMode = 0; _hasBaseline = NO; }
        }
    } else error = ENOTSUP;
    if (error) {
        if (_diagnostics < 32) {
            ++_diagnostics;
            fprintf(stderr, "[framebuffer] reject id=%d type=%u items=%u ports=%u error=%d\n",
                header->msgh_id, type, request->itemCount, request->descriptorCount, error);
            FBWireSwapchain chain;
            if (header->msgh_id == 9 && copyPayload(request, 7, &chain, sizeof(chain)))
                fprintf(stderr, "[framebuffer] requested swapchain width=%u height=%u display=%u chain=%u pixel=%u color=%u hdr=%u mode=%u\n",
                    chain.width, chain.height, chain.displayID, chain.swapchainID, chain.pixelFormat, chain.colorSpace, chain.hdr, chain.presentationMode);
            FBWirePresent present;
            if (header->msgh_id == 11 && copyPayload(request, 10, &present, sizeof(present)))
                fprintf(stderr, "[framebuffer] requested present op=%llu time=%llu flags=%llx source=%u,%u,%u,%u dest=%u,%u,%u,%u "
                    "display=%u chain=%u layer=%u\n",
                    (unsigned long long)present.operationID, (unsigned long long)present.desiredNanoseconds, (unsigned long long)present.flags,
                    present.source.x, present.source.y, present.source.width, present.source.height,
                    present.destination.x, present.destination.y, present.destination.width, present.destination.height,
                    present.displayID, present.swapchainID, present.layer);
        }
        beginReply(&reply, header->msgh_id, _connected ? _session : NULL);
        addError(&reply, error);
    }
    if (MACH_PORT_VALID(header->msgh_remote_port)) {
        mach_port_t port = header->msgh_remote_port;
        header->msgh_remote_port = MACH_PORT_NULL;
        if (sendReply(&reply, port, YES) && !error && header->msgh_id == 9) { _swapchain = 0; _presentationMode = 0; }
    }
}

- (int)present:(FBRequest *)request {
    if (_pending || _mainWork) return EAGAIN;
    FBWirePresent payload;
    if (!_swapchain || !copyPayload(request, 10, &payload, sizeof(payload))) return ENOTSUP;
    uint64_t now = monotonicNanoseconds();
    uint64_t delay = _presentationMode == 2 && payload.desiredNanoseconds > now ? payload.desiredNanoseconds - now : 0;
    uint32_t surfaceIndex = payload.surfaceDescriptorIndex, fenceIndex = payload.fenceDescriptorIndex;
    // One full-size BGRA layer, updated in place without scaling, translation, or blending.
    if (payload.displayID != 1 || payload.swapchainID != _swapchain || !payload.operationID || payload.operationID <= _lastOperation ||
        payload.layer || (payload.flags & ~UINT64_C(0x18)) || !(payload.flags & 8) || delay > 5 * NSEC_PER_SEC) return ENOTSUP;
    const FBWireRectangle rectangles[] = {payload.source, payload.destination};
    for (unsigned i = 0; i < 2; ++i) {
        const FBWireRectangle *r = &rectangles[i];
        if (!r->width || !r->height || r->x >= FBWidth || r->y >= FBHeight || r->width > FBWidth - r->x || r->height > FBHeight - r->y) return EINVAL;
    }
    if (memcmp(&payload.source, &payload.destination, sizeof(payload.source))) return ENOTSUP;
    BOOL fullFrame = !payload.source.x && !payload.source.y && payload.source.width == FBWidth && payload.source.height == FBHeight;
    if (!fullFrame && !_hasBaseline) return ENODATA;
    BOOL hasFence = fenceIndex != UINT32_MAX;
    if (request->descriptorCount != (hasFence ? 2u : 1u) || surfaceIndex >= request->descriptorCount ||
        request->descriptors[surfaceIndex].disposition != MACH_MSG_TYPE_PORT_SEND ||
        (hasFence && (fenceIndex >= request->descriptorCount || fenceIndex == surfaceIndex ||
                      request->descriptors[fenceIndex].disposition != MACH_MSG_TYPE_PORT_RECEIVE))) return EINVAL;
    LSBFramebufferPresent handler = self.frameHandler;
    if (!handler) return ENOTSUP;
    IOSurfaceRef surface = IOSurfaceLookupFromMachPort(request->descriptors[surfaceIndex].name);
    if (!surface) return EINVAL;
    size_t row = IOSurfaceGetBytesPerRow(surface), allocation = IOSurfaceGetAllocSize(surface);
    if (IOSurfaceGetID(surface) != payload.surfaceID || IOSurfaceGetWidth(surface) != FBWidth || IOSurfaceGetHeight(surface) != FBHeight ||
        IOSurfaceGetPlaneCount(surface) || IOSurfaceGetPixelFormat(surface) != 0x42475241 || IOSurfaceGetBytesPerElement(surface) != 4 ||
        row < 4 * FBWidth || row > SIZE_MAX / FBHeight || allocation < row * FBHeight) { CFRelease(surface); return ENOTSUP; }
    FBPresentation *presentation = [FBPresentation new];
    presentation->surface = surface;
    presentation->surfaceID = IOSurfaceGetID(surface);
    presentation->nativePID = (pid_t)_audit.val[5];
    presentation->dirtyRect = CGRectMake(payload.source.x, payload.source.y, payload.source.width, payload.source.height);
    presentation->fullFrame = fullFrame;
    presentation->operation = payload.operationID;
    presentation->desired = payload.desiredNanoseconds;
    presentation->chain = _swapchain;
    presentation->generation = _activeGeneration;
    presentation->callback = (payload.flags & 16) != 0;
    if (delay) {
        presentation->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _queue);
        if (!presentation->timer) { CFRelease(surface); presentation->surface = NULL; return ENOMEM; }
    }
    presentation->replyPort = request->header->msgh_remote_port;
    request->header->msgh_remote_port = MACH_PORT_NULL;
    if (hasFence) {
        presentation->fence = request->descriptors[fenceIndex].name;
        request->descriptors[fenceIndex].name = MACH_PORT_NULL;
    }
    _lastOperation = payload.operationID;
    _pending = presentation;
    if (presentation->timer) {
        __weak LSBFramebufferServer *weakSelf = self;
        __weak FBPresentation *weakPresentation = presentation;
        dispatch_source_set_event_handler(presentation->timer, ^{
            LSBFramebufferServer *server = weakSelf;
            FBPresentation *pending = weakPresentation;
            if (server && pending && server->_pending == pending) [server enqueuePresentation:pending handler:handler];
        });
        dispatch_source_set_timer(presentation->timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay), DISPATCH_TIME_FOREVER, 0);
        dispatch_resume(presentation->timer);
    } else [self enqueuePresentation:presentation handler:handler];
    return 0;
}

- (void)enqueuePresentation:(FBPresentation *)presentation handler:(LSBFramebufferPresent)handler {
    if (presentation->timer) { dispatch_source_cancel(presentation->timer); presentation->timer = nil; }
    if (atomic_load(&_closing) || presentation->generation != atomic_load(&_generation)) {
        atomic_store(&presentation->stage, 2);
        [self finish:presentation error:ECANCELED];
        return;
    }
    _mainWork = presentation;
    dispatch_async(dispatch_get_main_queue(), ^{
        unsigned queued = 0;
        if (!atomic_compare_exchange_strong(&presentation->stage, &queued, 1)) {
            dispatch_async(self->_queue, ^{ if (self->_mainWork == presentation) self->_mainWork = nil; });
            return;
        }
        LSBFramebufferCompletion completion = ^(int error) {
            unsigned active = 1;
            if (!atomic_compare_exchange_strong(&presentation->stage, &active, 2)) return;
            presentation->completed = monotonicNanoseconds();
            dispatch_async(self->_queue, ^{
                [self finish:presentation error:error];
                if (self->_mainWork == presentation) self->_mainWork = nil;
            });
        };
        if (!atomic_load(&self->_closing) && presentation->generation == atomic_load(&self->_generation)) {
            @try { handler(presentation->surface, presentation->dirtyRect, completion); }
            @catch (NSException *exception) { (void)exception; completion(EIO); }
        } else completion(ECANCELED);
    });
}

- (void)finish:(FBPresentation *)presentation error:(int)error {
    BOOL current = presentation->generation == atomic_load(&_generation) && !atomic_load(&_closing) && _connected;
    if (!current || atomic_load(&presentation->cancelled)) error = ECANCELED;
    if (presentation->timer) { dispatch_source_cancel(presentation->timer); presentation->timer = nil; }
    // Destroying the transferred receive right signals the producer's MACH_SEND_DEAD fence, after all host reads have ended.
    if (MACH_PORT_VALID(presentation->fence)) {
        mach_port_mod_refs(mach_task_self(), presentation->fence, MACH_PORT_RIGHT_RECEIVE, -1);
        presentation->fence = MACH_PORT_NULL;
    }
    if (presentation->surface) { CFRelease(presentation->surface); presentation->surface = NULL; }
    if (!error) {
        if (presentation->fullFrame) _hasBaseline = YES;
        ++_frames;
        if (!presentation->fullFrame) ++_partialFrames;
        if (_frames == 1 || !(_frames % 300) || (!presentation->fullFrame && _partialFrames == 1))
            fprintf(stderr, "[framebuffer] snapshot complete native=%d surface=%u operation=%llu frames=%u rect=%.0f,%.0f,%.0f,%.0f\n",
                presentation->nativePID, presentation->surfaceID, (unsigned long long)presentation->operation, _frames,
                presentation->dirtyRect.origin.x, presentation->dirtyRect.origin.y,
                presentation->dirtyRect.size.width, presentation->dirtyRect.size.height);
    }
    if (error && presentation->generation == _activeGeneration && _diagnostics < 32) {
        ++_diagnostics;
        fprintf(stderr, "[framebuffer] snapshot failed native=%d surface=%u operation=%llu error=%d\n", presentation->nativePID,
            presentation->surfaceID, (unsigned long long)presentation->operation, error);
    }
    if (MACH_PORT_VALID(presentation->replyPort)) {
        FBReply reply;
        beginReply(&reply, 11, current ? _session : NULL);
        if (error) addError(&reply, error);
        sendReply(&reply, presentation->replyPort, YES);
        presentation->replyPort = MACH_PORT_NULL;
    }
    if (current && presentation->callback) {
        FBReply callback;
        beginReply(&callback, 12, _session);
        FBWireCompletion payload = {
            .operationID = presentation->operation, .desiredNanoseconds = presentation->desired,
            .completedNanoseconds = presentation->completed, .swapchainID = presentation->chain
        };
        addItem(&callback, 11, &payload, sizeof(payload));
        if (error) addError(&callback, error);
        if (sendReply(&callback, _events, NO)) {
            uint64_t expected = presentation->generation;
            if (atomic_compare_exchange_strong(&_generation, &expected, presentation->generation + 1)) {
                _activeGeneration = presentation->generation + 1;
                _authorized = NO;
                [self dropClient];
            }
        }
    }
    if (_pending == presentation) _pending = nil;
}
@end
