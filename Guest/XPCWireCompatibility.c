#include "XPCWireCompatibility.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#if defined(LSB_TRANSPORT_TRACE)
#include <stdio.h>
#endif

enum { SimulatorMagic = 0x7870633f, NativeMagic = 0x40585043, WireVersion = 5 };
enum { PipeRequestID = 0x40000000, PipeReplyID = 0x20000000, MaximumGraphBytes = 2 * 1024 * 1024 };

typedef struct { _Atomic(uintptr_t) owner; mach_port_t broker, reply; bool translating; } WireContext;
static WireContext contexts[16];
static _Atomic(unsigned) activeContexts;
#if defined(LSB_XPC_WIRE_TESTING)
static LSBXPCWireStatistics statistics;
#endif

static WireContext *findContext(uintptr_t owner) {
    for (unsigned i = 0; i < sizeof(contexts) / sizeof(contexts[0]); ++i)
        if (atomic_load_explicit(&contexts[i].owner, memory_order_acquire) == owner) return &contexts[i];
    return NULL;
}

int LSBXPCWireBegin(mach_port_t broker) {
    if (!MACH_PORT_VALID(broker)) return EINVAL;
    uintptr_t owner = (uintptr_t)pthread_self();
    if (findContext(owner)) return EBUSY;
    for (unsigned i = 0; i < sizeof(contexts) / sizeof(contexts[0]); ++i) {
        uintptr_t empty = 0;
        if (!atomic_compare_exchange_strong_explicit(&contexts[i].owner, &empty, UINTPTR_MAX, memory_order_acquire, memory_order_relaxed)) continue;
        contexts[i].broker = broker;
        contexts[i].reply = MACH_PORT_NULL;
        contexts[i].translating = false;
        atomic_store_explicit(&contexts[i].owner, owner, memory_order_release);
        atomic_fetch_add_explicit(&activeContexts, 1, memory_order_release);
        return 0;
    }
    return EAGAIN;
}

void LSBXPCWireEnd(void) {
    WireContext *context = findContext((uintptr_t)pthread_self());
    if (!context) return;
    atomic_store_explicit(&context->owner, 0, memory_order_release);
    atomic_fetch_sub_explicit(&activeContexts, 1, memory_order_release);
}

// XPC v5 uses either an inline graph following port descriptors, or a first OOL descriptor followed only by port descriptors.
// Validate the complete descriptor list before accessing a graph. VM calls also reject inaccessible/read-only OOL addresses safely.
static int translateGraph(mach_msg_header_t *message, size_t capacity, uint32_t from, uint32_t to, bool *outOfLine) {
    if (!message || capacity < sizeof(*message) || message->msgh_size < sizeof(*message) || message->msgh_size > capacity) return EPROTO;
    size_t offset = sizeof(*message), size = message->msgh_size;
    uintptr_t graph = 0;
    size_t graphSize = 0;
    *outOfLine = false;
    if (message->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
        if (size - offset < sizeof(mach_msg_body_t)) return EPROTO;
        mach_msg_body_t body;
        memcpy(&body, (char *)message + offset, sizeof(body));
        offset += sizeof(body);
        if (!body.msgh_descriptor_count || body.msgh_descriptor_count > (size - offset) / sizeof(mach_msg_port_descriptor_t)) return EPROTO;
        for (unsigned i = 0; i < body.msgh_descriptor_count; ++i) {
            if (size - offset < sizeof(mach_msg_type_descriptor_t)) return EPROTO;
            mach_msg_type_descriptor_t type;
            memcpy(&type, (char *)message + offset, sizeof(type));
            size_t descriptorSize;
            if (type.type == MACH_MSG_PORT_DESCRIPTOR) descriptorSize = sizeof(mach_msg_port_descriptor_t);
            else if (!i && type.type == MACH_MSG_OOL_DESCRIPTOR) {
                descriptorSize = sizeof(mach_msg_ool_descriptor_t);
                if (size - offset < descriptorSize) return EPROTO;
                mach_msg_ool_descriptor_t descriptor;
                memcpy(&descriptor, (char *)message + offset, sizeof(descriptor));
                graph = (uintptr_t)descriptor.address;
                graphSize = descriptor.size;
                if (!graph || graphSize < 8 || graphSize > MaximumGraphBytes || graph > UINTPTR_MAX - graphSize) return EPROTO;
                *outOfLine = true;
            } else return EPROTO;
            if (size - offset < descriptorSize) return EPROTO;
            offset += descriptorSize;
        }
    }
    if (!*outOfLine) { graph = (uintptr_t)message + offset; graphSize = size - offset; }
    if (graphSize < 8 || graphSize > MaximumGraphBytes) return EPROTO;
    uint32_t header[2];
    if (*outOfLine) {
        vm_size_t copied = 0;
        if (vm_read_overwrite(mach_task_self(), graph, sizeof(header), (vm_address_t)header, &copied) || copied != sizeof(header)) return EFAULT;
    } else memcpy(header, (const void *)graph, sizeof(header));
    if (header[0] != from || header[1] != WireVersion) return EPROTO;
    if (*outOfLine) {
        if (vm_write(mach_task_self(), graph, (vm_offset_t)&to, sizeof(to))) return EFAULT;
    } else memcpy((void *)graph, &to, sizeof(to));
    return 0;
}

static mach_msg_return_t brokerMachMessage(mach_msg_header_t *message, mach_msg_option_t options, mach_msg_size_t sendSize,
                                          mach_msg_size_t receiveSize, mach_port_name_t receivePort, mach_msg_timeout_t timeout,
                                          mach_port_name_t notify) {
    // Interposition is installed before libSystem initializes. Do not touch TLS, malloc, or pthread state until Begin publishes a context.
    if (!atomic_load_explicit(&activeContexts, memory_order_acquire))
        return mach_msg(message, options, sendSize, receiveSize, receivePort, timeout, notify);
    WireContext *context = findContext((uintptr_t)pthread_self());
    if (!context || context->translating) return mach_msg(message, options, sendSize, receiveSize, receivePort, timeout, notify);
    mach_port_t destination = message ? message->msgh_remote_port : MACH_PORT_NULL;
    bool sending = (options & MACH_SEND_MSG) && destination == context->broker;
    if (sending) {
        bool outOfLine = false;
        if (!message || sendSize < sizeof(*message) || message->msgh_id != PipeRequestID ||
            !(options & MACH_RCV_MSG) || !MACH_PORT_VALID(receivePort) || message->msgh_local_port != receivePort) return MACH_SEND_INVALID_DATA;
#if defined(LSB_XPC_WIRE_TESTING)
        // The test executable uses native libxpc. Model the simulator's encoding before exercising the production adapter.
        context->translating = true;
        int simulationError = translateGraph(message, sendSize, NativeMagic, SimulatorMagic, &outOfLine);
        context->translating = false;
        if (simulationError) {
#if defined(LSB_TRANSPORT_TRACE)
            fprintf(stderr, "LiveSimBooter XPC: test encoding failed: error=%d size=%u bits=0x%x\n", simulationError, sendSize, message->msgh_bits);
#endif
            return MACH_SEND_INVALID_DATA;
        }
#endif
        context->translating = true;
        int error = translateGraph(message, sendSize, SimulatorMagic, NativeMagic, &outOfLine);
        context->translating = false;
        if (error) {
#if defined(LSB_TRANSPORT_TRACE)
            fprintf(stderr, "LiveSimBooter XPC: request wire translation failed: error=%d\n", error);
#endif
            return MACH_SEND_INVALID_DATA;
        }
        context->reply = receivePort;
#if defined(LSB_XPC_WIRE_TESTING)
        if (outOfLine) ++statistics.sentOOL; else ++statistics.sentInline;
#endif
    }
    bool receiving = (options & MACH_RCV_MSG) && (!(options & MACH_SEND_MSG) || sending) &&
                     MACH_PORT_VALID(context->reply) && receivePort == context->reply;
    // dyld leaves this image's own import bound to the original function; no cache code or state is patched.
    mach_msg_return_t result = mach_msg(message, options, sendSize, receiveSize, receivePort, timeout, notify);
    if (receiving && !result && message->msgh_id == PipeReplyID) {
        bool outOfLine = false;
        context->translating = true;
        int error = translateGraph(message, receiveSize, NativeMagic, SimulatorMagic, &outOfLine);
#if defined(LSB_XPC_WIRE_TESTING)
        if (!error) {
            if (outOfLine) ++statistics.receivedOOL; else ++statistics.receivedInline;
            // Let the native test client's parser consume the translated simulator reply.
            error = translateGraph(message, receiveSize, SimulatorMagic, NativeMagic, &outOfLine);
        }
#endif
        context->translating = false;
        // Leave malformed replies to libxpc, which owns and destroys their received descriptors on rejection.
#if defined(LSB_TRANSPORT_TRACE)
        if (error) fprintf(stderr, "LiveSimBooter XPC: reply wire translation failed: error=%d\n", error);
#else
        (void)error;
#endif
    }
#if defined(LSB_XPC_WIRE_TESTING)
    if (receiving && result == MACH_RCV_TOO_LARGE) ++statistics.receiveRetries;
#endif
#if defined(LSB_TRANSPORT_TRACE)
    if (result) {
        int savedErrno = errno;
        fprintf(stderr, "LiveSimBooter Mach: mach_msg failed: result=0x%08x options=0x%08x destination=%u receive=%u\n",
                (unsigned)result, (unsigned)options, destination, receivePort);
        errno = savedErrno;
    }
#endif
    return result;
}

__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interposition = {
    (const void *)&brokerMachMessage, (const void *)&mach_msg
};

#if defined(LSB_XPC_WIRE_TESTING)
LSBXPCWireStatistics LSBXPCWireTestStatistics(void) { return statistics; }
int LSBXPCWireTestTranslate(mach_msg_header_t *message, size_t capacity, unsigned from, unsigned to) {
    bool outOfLine;
    return translateGraph(message, capacity, from, to, &outOfLine);
}
#endif
