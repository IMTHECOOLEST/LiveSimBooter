#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/arm/thread_status.h>
#include <mach/exc.h>
#include <mach/mig_errors.h>
#include <mach/ndr.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned destroyCalls, getCalls, setCalls, legacyCalls, rpcCalls, signCalls, originalCalls, modRefCalls;
static unsigned expectedBehavior, malformedReply;
static kern_return_t forwardResult, getResult, setResult;
static uintptr_t signedStart;
static uint32_t signedFlags;
static arm_thread_state64_t kernelState;
static const mach_port_t workerPort = 0x170b, handlerPort = 0x220b, replyPort = 0x330b;
static mach_msg_return_t testMachMsg(mach_msg_header_t *, mach_msg_option_t, mach_msg_size_t, mach_msg_size_t,
                                    mach_port_name_t, mach_msg_timeout_t, mach_port_name_t);

static void testDestroy(mach_msg_header_t *header) {
    ++destroyCalls;
    assert(header->msgh_remote_port == MACH_PORT_NULL);
}
static mach_port_t testReplyPort(void) { return replyPort; }
static kern_return_t testModRefs(ipc_space_t task, mach_port_name_t port, mach_port_right_t right, mach_port_delta_t delta) {
    assert(task == mach_task_self() && port == replyPort && right == MACH_PORT_RIGHT_RECEIVE && delta == -1);
    ++modRefCalls;
    return KERN_SUCCESS;
}
static kern_return_t testGet(thread_act_t port, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t *count) {
    assert(port == workerPort && flavor == ARM_THREAD_STATE64 && *count >= ARM_THREAD_STATE64_COUNT);
    ++getCalls;
    memcpy(state, &kernelState, sizeof(kernelState));
    *count = ARM_THREAD_STATE64_COUNT;
    return getResult;
}
static kern_return_t testSet(thread_act_t port, thread_state_flavor_t flavor, thread_state_t state, mach_msg_type_number_t count) {
    assert(port == workerPort && flavor == ARM_THREAD_STATE64 && count == ARM_THREAD_STATE64_COUNT);
    ++setCalls;
    memcpy(&kernelState, state, sizeof(kernelState));
    return setResult;
}
static kern_return_t testRaise(mach_port_t port, mach_port_t thread, mach_port_t task, exception_type_t exception,
                              exception_data_t code, mach_msg_type_number_t count) {
    assert(port == handlerPort && thread == workerPort && task == mach_task_self() && exception == EXC_BAD_ACCESS);
    assert(count == 2 && code[0] == KERN_INVALID_ADDRESS && (uint32_t)code[1] == 0x21218a74);
    ++legacyCalls;
    kernelState.__pc = 0x100040000;
    return forwardResult;
}
static kern_return_t testRaiseState(mach_port_t port, exception_type_t exception, const exception_data_t code,
                                   mach_msg_type_number_t count, int *flavor, const thread_state_t oldState,
                                   mach_msg_type_number_t oldCount, thread_state_t newState, mach_msg_type_number_t *newCount) {
    assert(port == handlerPort && exception == EXC_BAD_ACCESS && count == 2 && code[0] == KERN_INVALID_ADDRESS);
    assert(*flavor == ARM_THREAD_STATE64 && oldCount == ARM_THREAD_STATE64_COUNT && *newCount >= oldCount);
    ++legacyCalls;
    memcpy(newState, oldState, sizeof(kernelState));
    ((arm_thread_state64_t *)newState)->__pc = 0x100050000;
    *newCount = oldCount;
    return forwardResult;
}
static kern_return_t testRaiseIdentity(mach_port_t port, mach_port_t thread, mach_port_t task, exception_type_t exception,
                                      exception_data_t code, mach_msg_type_number_t count, int *flavor, thread_state_t oldState,
                                      mach_msg_type_number_t oldCount, thread_state_t newState, mach_msg_type_number_t *newCount) {
    assert(thread == workerPort && task == mach_task_self());
    return testRaiseState(port, exception, code, count, flavor, oldState, oldCount, newState, newCount);
}
static int testSupported(void) { ++originalCalls; return 0x37; }
static int testOpen(void) { ++originalCalls; return 23; }
uintptr_t LSBThreadSignPC(uintptr_t start, uint32_t flags) {
    ++signCalls;
    signedStart = start;
    signedFlags = flags;
    return start | (flags & 1 ? 0 : UINT64_C(0x4321000000000000));
}

#define LSB_WORKQUEUE_BOOTSTRAP_TESTING
#define mach_msg testMachMsg
#define mach_msg_destroy testDestroy
#define mach_reply_port testReplyPort
#define mach_port_mod_refs testModRefs
#define thread_get_state testGet
#define thread_set_state testSet
#define exception_raise testRaise
#define exception_raise_state testRaiseState
#define exception_raise_state_identity testRaiseIdentity
#define _pthread_workqueue_supported testSupported
#define __workq_open testOpen
#include "../Guest/WorkqueueBootstrap.c"
#undef mach_msg
#undef mach_msg_destroy
#undef mach_reply_port
#undef mach_port_mod_refs
#undef thread_get_state
#undef thread_set_state
#undef exception_raise
#undef exception_raise_state
#undef exception_raise_state_identity
#undef _pthread_workqueue_supported
#undef __workq_open

static void reset(void) {
    destroyCalls = getCalls = setCalls = legacyCalls = rpcCalls = signCalls = originalCalls = modRefCalls = 0;
    malformedReply = 0;
    forwardResult = getResult = setResult = KERN_SUCCESS;
    memset(&kernelState, 0, sizeof(kernelState));
    memset(savedPorts, 0, sizeof(savedPorts));
    savedCount = 0;
    nativeEntry = 0x2321218a74;
    guestEntry = 0x12b269a74;
    LSBWorkqueueRegister(guestEntry);
}

static Message requestMessage(void) {
    Message message = {0};
    IdentityPrefix prefix = {0};
    prefix.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
    prefix.header.msgh_remote_port = replyPort;
    prefix.header.msgh_id = StateIdentityID;
    prefix.body.msgh_descriptor_count = 2;
    prefix.thread = (mach_msg_port_descriptor_t){.name = workerPort, .disposition = MACH_MSG_TYPE_PORT_SEND, .type = MACH_MSG_PORT_DESCRIPTOR};
    prefix.task = (mach_msg_port_descriptor_t){.name = mach_task_self(), .disposition = MACH_MSG_TYPE_PORT_SEND, .type = MACH_MSG_PORT_DESCRIPTOR};
    prefix.ndr = NDR_record;
    prefix.exception = EXC_BAD_ACCESS;
    prefix.codeCount = 2;
    int64_t code[] = {KERN_INVALID_ADDRESS, (int64_t)nativeEntry};
    int flavor = ARM_THREAD_STATE64;
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    arm_thread_state64_t state;
    memset(&state, 0x3c, sizeof(state));
    state.__pc = nativeEntry;
    state.__pad = StateNoPtrauth;
    state.__x[0] = 0x104008000;
    state.__x[1] = workerPort;
    state.__x[2] = 0x104000000;
    state.__x[3] = 0;
    state.__x[4] = WorkqueueTSDBaseSet | 0x40000;
    state.__x[5] = 0;
    state.__sp = 0x104008000;
    size_t size = 0;
    append(&message, &size, &prefix, sizeof(prefix));
    append(&message, &size, code, sizeof(code));
    append(&message, &size, &flavor, 4);
    append(&message, &size, &count, 4);
    append(&message, &size, &state, sizeof(state));
    message.header.msgh_size = (mach_msg_size_t)size;
    assert(size == 364);
    return message;
}

static void metadataTests(void) {
    reset();
    LSBWorkqueueRegister(0);
    assert(LSBWorkqueueSupported() == 0x37 && originalCalls == 1);
    assert(LSBWorkqueueOpen() == 23 && originalCalls == 2);
    LSBWorkqueueRegister(guestEntry);
    unsetenv(LSB_HOST_WORKQUEUE_START_ENV);
    unsetenv(LSB_HOST_WORKQUEUE_FEATURES_ENV);
    assert(!LSBWorkqueueSupported());
    setenv(LSB_HOST_WORKQUEUE_START_ENV, "0x2321218a74", 1);
    setenv(LSB_HOST_WORKQUEUE_FEATURES_ENV, "0xff", 1);
    errno = EDOM;
    assert(LSBWorkqueueSupported() == 0xff && errno == EDOM && originalCalls == 2);
    const char *invalid[] = {"", "-1", "0", "0x2321218a75", "0x2321218a74junk", "18446744073709551616"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        setenv(LSB_HOST_WORKQUEUE_START_ENV, invalid[i], 1);
        assert(!LSBWorkqueueSupported());
    }
    setenv(LSB_HOST_WORKQUEUE_START_ENV, "0x2321218a74", 1);
    setenv(LSB_HOST_WORKQUEUE_FEATURES_ENV, "0x80000000", 1);
    assert(!LSBWorkqueueSupported());
}

static void parseTests(void) {
    reset();
    Message original = requestMessage();
    ExceptionRequest request;
    assert(parseException(&original, &request));
    assert(request.thread == workerPort && request.task == mach_task_self() && request.code[1] == (int64_t)nativeEntry);
    for (unsigned variation = 0; variation < 11; ++variation) {
        Message message = original;
        IdentityPrefix *prefix = (IdentityPrefix *)&message;
        switch (variation) {
            case 0: message.header.msgh_id = 2403; break;
            case 1: message.header.msgh_bits &= ~MACH_MSGH_BITS_COMPLEX; break;
            case 2: --message.header.msgh_size; break;
            case 3: prefix->body.msgh_descriptor_count = 1; break;
            case 4: prefix->thread.type = MACH_MSG_OOL_DESCRIPTOR; break;
            case 5: prefix->task.disposition = MACH_MSG_TYPE_COPY_SEND; break;
            case 6: prefix->task.name = MACH_PORT_NULL; break;
            case 7: prefix->codeCount = 3; break;
            case 8: prefix->ndr.int_rep ^= 1; break;
            case 9: *(uint32_t *)(message.bytes + 84) = ARM_EXCEPTION_STATE64; break;
            case 10: *(uint32_t *)(message.bytes + 88) = 67; break;
        }
        assert(!parseException(&message, &request));
    }
}

static void redirectTests(void) {
    reset();
    Message message = requestMessage();
    ExceptionRequest original;
    assert(parseException(&message, &original));
    for (unsigned mode = 0; mode < 4; ++mode) {
        ExceptionRequest request = original;
        if (mode == 1) { request.state.__x[4] = WorkqueueReuse; request.state.__x[5] = -1; }
        if (mode == 2) { request.state.__x[4] = WorkqueueReuse; request.state.__x[5] = 16; }
        if (mode == 3) {
            request.state.__pad = 0x6a00000c;
            request.state.__pc |= UINT64_C(0x4321000000000000);
            request.state.__sp |= UINT64_C(0x4311000000000000);
        }
        arm_thread_state64_t expected = request.state;
        assert(redirectException(&request));
        expected.__pc = guestEntry | (expected.__pad & StateNoPtrauth ? 0 : UINT64_C(0x4321000000000000));
        expected.__pad &= ~StateKernelSignedPC;
        assert(!memcmp(&request.state, &expected, sizeof(expected)) && signedStart == guestEntry);
    }
    assert(signCalls == 4 && signedFlags == 0x6a00000c);
    for (unsigned variation = 0; variation < 11; ++variation) {
        ExceptionRequest request = original;
        switch (variation) {
            case 0: ++request.state.__pc; break;
            case 1: ++request.code[1]; break;
            case 2: request.code[0] = KERN_NO_ACCESS; break;
            case 3: request.codeCount = 1; break;
            case 4: request.state.__x[1] = 0; break;
            case 5: request.state.__x[0] |= 1; break;
            case 6: request.state.__x[2] = request.state.__sp + 16; break;
            case 7: request.state.__sp = request.state.__x[0] + 16; break;
            case 8: request.state.__x[4] = 0; break;
            case 9: request.state.__x[5] = 17; break;
            case 10: request.state.__x[5] = -2; break;
        }
        assert(!redirectException(&request));
    }
    StateReply reply;
    handleMessage(&message, &reply);
    assert(destroyCalls == 1 && reply.result == KERN_SUCCESS && reply.header.msgh_remote_port == replyPort);
    assert(reply.header.msgh_id == 2507 && reply.header.msgh_size == 316 && reply.stateCount == ARM_THREAD_STATE64_COUNT);
    arm_thread_state64_t replyState;
    memcpy(&replyState, reply.state, sizeof(replyState));
    assert(replyState.__pc == guestEntry);
}

static mach_msg_return_t testMachMsg(mach_msg_header_t *header, mach_msg_option_t options, mach_msg_size_t sendSize,
                                    mach_msg_size_t receiveSize, mach_port_name_t receivePort, mach_msg_timeout_t timeout, mach_port_name_t notify) {
    assert(options == (MACH_SEND_MSG | MACH_RCV_MSG) && receiveSize == MessageCapacity && receivePort == replyPort);
    assert(timeout == MACH_MSG_TIMEOUT_NONE && notify == MACH_PORT_NULL && header->msgh_remote_port == handlerPort);
    assert(header->msgh_local_port == replyPort && header->msgh_id == 2404 + (int)expectedBehavior && header->msgh_size == sendSize);
    ++rpcCalls;
    bool identity = expectedBehavior != EXCEPTION_STATE, hasState = expectedBehavior != EXCEPTION_DEFAULT;
    assert(!!(header->msgh_bits & MACH_MSGH_BITS_COMPLEX) == identity);
    size_t offset = sizeof(mach_msg_header_t);
    Message *message = (Message *)header;
    if (identity) {
        IdentityPrefix *prefix = (IdentityPrefix *)message;
        assert(prefix->body.msgh_descriptor_count == 2);
        assert(prefix->thread.name == workerPort && prefix->thread.disposition == MACH_MSG_TYPE_COPY_SEND);
        assert(prefix->task.name == mach_task_self() && prefix->task.disposition == MACH_MSG_TYPE_COPY_SEND);
        offset += sizeof(mach_msg_body_t) + 2 * sizeof(mach_msg_port_descriptor_t);
    }
    assert(!memcmp(message->bytes + offset, &NDR_record, sizeof(NDR_record)));
    offset += sizeof(NDR_record);
    assert(*(int32_t *)(message->bytes + offset) == EXC_BAD_ACCESS && *(uint32_t *)(message->bytes + offset + 4) == 2);
    int64_t address;
    memcpy(&address, message->bytes + offset + 16, 8);
    assert(address == (int64_t)nativeEntry);
    offset += 24;
    assert(sendSize == offset + (hasState ? 8 + sizeof(kernelState) : 0));
    memset(message, 0, sizeof(*message));
    StateReply *reply = (StateReply *)message;
    reply->header.msgh_id = 2504 + (int)expectedBehavior;
    reply->header.msgh_size = sizeof(mig_reply_error_t);
    reply->ndr = NDR_record;
    reply->result = forwardResult;
    if (hasState && !forwardResult) {
        reply->flavor = ARM_THREAD_STATE64;
        reply->stateCount = ARM_THREAD_STATE64_COUNT;
        arm_thread_state64_t replyState = kernelState;
        replyState.__pc = 0x100060000;
        memcpy(reply->state, &replyState, sizeof(replyState));
        reply->header.msgh_size = offsetof(StateReply, state) + sizeof(kernelState);
    } else if (!forwardResult) kernelState.__pc = 0x100070000;
    if (malformedReply == 1) ++reply->header.msgh_id;
    if (malformedReply == 2) --reply->header.msgh_size;
    if (malformedReply == 3) reply->stateCount = THREAD_STATE_MAX + 1;
    return KERN_SUCCESS;
}

static void forwardingTests(void) {
    for (unsigned wide = 0; wide < 2; ++wide) for (unsigned behavior = 1; behavior <= 3; ++behavior) {
        reset();
        Message message = requestMessage();
        ExceptionRequest request;
        assert(parseException(&message, &request));
        kernelState = request.state;
        savedCount = 1;
        savedMasks[0] = EXC_MASK_BAD_ACCESS;
        savedPorts[0] = handlerPort;
        savedBehaviors[0] = behavior | (wide ? MACH_EXCEPTION_CODES : 0);
        savedFlavors[0] = ARM_THREAD_STATE64;
        expectedBehavior = behavior;
        assert(forwardException(&request) == KERN_SUCCESS);
        uintptr_t expectedPC = wide ? (behavior == EXCEPTION_DEFAULT ? 0x100070000 : 0x100060000) :
                                      (behavior == EXCEPTION_DEFAULT ? 0x100040000 : 0x100050000);
        assert(request.state.__pc == expectedPC && getCalls == (behavior == EXCEPTION_DEFAULT ? 1u : 2u));
        assert(setCalls == (behavior != EXCEPTION_DEFAULT) && rpcCalls == wide && legacyCalls == !wide && modRefCalls == wide);
        if (wide) for (unsigned malformed = 1; malformed <= (behavior == EXCEPTION_DEFAULT ? 2u : 3u); ++malformed) {
            malformedReply = malformed;
            assert(forwardException(&request) != KERN_SUCCESS);
        }
        malformedReply = 0;
        forwardResult = KERN_FAILURE;
        assert(forwardException(&request) == KERN_FAILURE);
    }
    reset();
    Message message = requestMessage();
    ExceptionRequest request;
    assert(parseException(&message, &request));
    assert(forwardException(&request) == KERN_FAILURE);
    assert(!supportedBehavior(EXCEPTION_IDENTITY_PROTECTED, ARM_THREAD_STATE64));
    assert(!supportedBehavior(EXCEPTION_DEFAULT | MACH_EXCEPTION_BACKTRACE_PREFERRED, ARM_THREAD_STATE64));
    assert(!supportedBehavior(EXCEPTION_STATE, THREAD_STATE_NONE));
}

int main(void) {
    metadataTests();
    parseTests();
    redirectTests();
    forwardingTests();
    puts("WorkqueueBootstrap tests passed");
    return 0;
}
