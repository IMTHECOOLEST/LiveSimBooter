#include "../LiveSimBooter/LocalMach.cpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

namespace {
struct Trap {
    int64_t number;
    uintptr_t args[8];
};
std::vector<Trap> calls;
std::function<int64_t(const Trap &)> handle;
constexpr mach_port_t kTask = 0x1003;
constexpr mach_port_t kReply = 0x2003;

void reset()
{
    calls.clear();
    handle = {};
}

void simpleReply(mach_msg_header_t *header, kern_return_t result = KERN_SUCCESS)
{
    auto id = header->msgh_id;
    std::memset(header, 0, sizeof(mig_reply_error_t));
    auto *reply = reinterpret_cast<mig_reply_error_t *>(header);
    reply->Head.msgh_id = id + 100;
    reply->Head.msgh_size = sizeof(*reply);
    reply->RetCode = result;
}

void testFastTraps()
{
    reset();
    vm_address_t address = 0;
    handle = [&](const Trap &trap) -> int64_t {
        if (trap.number == -10) {
            assert(trap.args[0] == kTask && trap.args[2] == 16384 && trap.args[3] == VM_FLAGS_ANYWHERE);
            *reinterpret_cast<vm_address_t *>(trap.args[1]) = 0x400000000;
        }
        return KERN_SUCCESS;
    };
    assert(lsb::vm_allocate(kTask, &address, 16384, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    assert(address == 0x400000000);
    assert(lsb::vm_protect(kTask, address, 16384, false, VM_PROT_READ) == KERN_SUCCESS);
    assert(calls.back().number == -14 && calls.back().args[4] == VM_PROT_READ);
    assert(lsb::vm_deallocate(kTask, address, 16384) == KERN_SUCCESS);
    assert(calls.back().number == -12);
    handle = [](const Trap &trap) -> int64_t { return trap.number == -3 ? INT64_C(0x123456789abcdef) : KERN_INVALID_ARGUMENT; };
    assert(lsb::mach_absolute_time() == UINT64_C(0x123456789abcdef));
    mach_timebase_info_data_t timebase;
    assert(lsb::mach_timebase_info(&timebase) == KERN_INVALID_ARGUMENT);
    assert(calls.back().number == -89 && calls.back().args[0] == reinterpret_cast<uintptr_t>(&timebase));
}

void testInterruptedReceiveNeverResends()
{
    reset();
    mach_msg_header_t header = {};
    header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
    header.msgh_remote_port = 123;
    header.msgh_local_port = 456;
    header.msgh_voucher_port = 789;
    header.msgh_id = 42;
    unsigned messages = 0;
    handle = [&](const Trap &trap) -> int64_t {
        assert(trap.number == -47);
        assert((trap.args[1] & UINT64_C(0x400000000)) != 0);
        assert(trap.args[3] == (UINT64_C(456) << 32 | 123));
        assert(trap.args[4] == (UINT64_C(42) << 32 | 789));
        assert(trap.args[5] == (UINT64_C(456) << 32));
        assert((trap.args[6] & UINT32_MAX) == 1024);
        ++messages;
        if (messages == 1)
            return MACH_SEND_INTERRUPTED;
        if (messages == 2)
            return MACH_RCV_INTERRUPTED;
        assert(!(trap.args[1] & MACH_SEND_MSG));
        assert(!(trap.args[1] & MACH_RCV_SYNC_WAIT));
        assert((trap.args[2] >> 32) == 0);
        return MACH_MSG_SUCCESS;
    };
    assert(lsb::mach_msg(&header, MACH_SEND_MSG | MACH_RCV_MSG | MACH_RCV_SYNC_WAIT, sizeof(header), 1024, 456, 0, 0) == MACH_MSG_SUCCESS);
    assert(messages == 3);
    messages = 0;
    assert(lsb::mach_msg(&header, MACH_SEND_MSG | MACH_SEND_INTERRUPT, sizeof(header), 1024, 456, 0, 0) == MACH_SEND_INTERRUPTED);
    assert(messages == 1);
}

void testVMRPCAndKernelError()
{
    reset();
    handle = [](const Trap &trap) -> int64_t {
        assert(trap.number == -47);
        assert((trap.args[1] & UINT64_C(0x200000000)) != 0);
        auto *request = reinterpret_cast<__Request__vm_copy_t *>(trap.args[0]);
        assert(request->Head.msgh_id == 4807);
        assert(request->Head.msgh_size == sizeof(*request));
        assert(request->Head.msgh_remote_port == kTask && request->Head.msgh_local_port == kReply);
        assert(request->source_address == 0x100000000 && request->dest_address == 0x200000000 && request->size == 16384);
        assert(request->NDR.int_rep == 1);
        simpleReply(&request->Head, KERN_PROTECTION_FAILURE);
        return MACH_MSG_SUCCESS;
    };
    assert(lsb::vm_copy(kTask, 0x100000000, 16384, 0x200000000) == KERN_PROTECTION_FAILURE);
    assert(calls.back().number == -19 && calls.back().args[1] == kReply && calls.back().args[2] == MACH_PORT_RIGHT_RECEIVE);
}

void testVMFallbackUses64BitSubsystem()
{
    reset();
    vm_address_t address = 0;
    handle = [](const Trap &trap) -> int64_t {
        if (trap.number == -10)
            return MACH_SEND_INVALID_DEST;
        assert(trap.number == -47);
        auto *request = reinterpret_cast<__Request__vm_allocate_t *>(trap.args[0]);
        assert(request->Head.msgh_id == 4800 && request->Head.msgh_size == 52);
        assert(request->address == 0 && request->size == 16384 && request->flags == VM_FLAGS_ANYWHERE);
        auto *reply = reinterpret_cast<__Reply__vm_allocate_t *>(request);
        simpleReply(&reply->Head);
        reply->Head.msgh_size = sizeof(*reply);
        reply->address = 0x12345678000;
        return MACH_MSG_SUCCESS;
    };
    assert(lsb::vm_allocate(kTask, &address, 16384, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    assert(address == 0x12345678000);
}

void testTaskInfoValidation()
{
    reset();
    bool oversized = false;
    handle = [&](const Trap &trap) -> int64_t {
        auto *request = reinterpret_cast<__Request__task_info_t *>(trap.args[0]);
        assert(request->Head.msgh_id == 3405 && request->flavor == TASK_BASIC_INFO_64 && request->task_info_outCnt == 4);
        auto *reply = reinterpret_cast<__Reply__task_info_t *>(request);
        simpleReply(&reply->Head);
        reply->Head.msgh_size = offsetof(__Reply__task_info_t, task_info_out) + 3 * sizeof(integer_t);
        reply->task_info_outCnt = oversized ? 95 : 3;
        reply->task_info_out[0] = 11;
        reply->task_info_out[1] = 22;
        reply->task_info_out[2] = 33;
        return MACH_MSG_SUCCESS;
    };
    integer_t info[4] = {};
    mach_msg_type_number_t count = 4;
    assert(lsb::task_info(kTask, TASK_BASIC_INFO_64, info, &count) == KERN_SUCCESS);
    assert(count == 3 && info[0] == 11 && info[1] == 22 && info[2] == 33 && info[3] == 0);
    oversized = true;
    count = 4;
    assert(lsb::task_info(kTask, TASK_BASIC_INFO_64, info, &count) == MIG_TYPE_ERROR);
}

void testImageRegistrationAndOwnership()
{
    reset();
    dyld_kernel_image_info image = {};
    handle = [&](const Trap &trap) -> int64_t {
        auto *request = reinterpret_cast<__Request__task_register_dyld_image_infos_t *>(trap.args[0]);
        assert(request->Head.msgh_id == 3444);
        assert(request->Head.msgh_bits & MACH_MSGH_BITS_COMPLEX);
        assert(request->msgh_body.msgh_descriptor_count == 1);
        assert(request->dyld_images.address == &image && request->dyld_images.size == sizeof(image));
        assert(request->dyld_images.copy == MACH_MSG_VIRTUAL_COPY && !request->dyld_images.deallocate);
        assert(request->dyld_imagesCnt == 1 && (trap.args[5] & UINT32_MAX) == 1);
        simpleReply(&request->Head);
        return MACH_MSG_SUCCESS;
    };
    assert(lsb::task_register_dyld_image_infos(kTask, &image, 1) == KERN_SUCCESS);
    handle = [&](const Trap &trap) -> int64_t {
        auto *reply = reinterpret_cast<__Reply__task_get_dyld_image_infos_t *>(trap.args[0]);
        assert(reply->Head.msgh_id == 3446);
        std::memset(reply, 0, sizeof(*reply));
        reply->Head.msgh_id = 3546;
        reply->Head.msgh_size = sizeof(*reply);
        reply->Head.msgh_bits = MACH_MSGH_BITS_COMPLEX;
        reply->msgh_body.msgh_descriptor_count = 1;
        reply->dyld_images.type = MACH_MSG_OOL_DESCRIPTOR;
        reply->dyld_images.address = &image;
        reply->dyld_images.size = sizeof(image);
        reply->dyld_images.deallocate = true;
        reply->dyld_imagesCnt = 1;
        return MACH_MSG_SUCCESS;
    };
    dyld_kernel_image_info_array_t images = nullptr;
    mach_msg_type_number_t count = 0;
    assert(lsb::task_get_dyld_image_infos(kTask, &images, &count) == KERN_SUCCESS);
    assert(images == &image && count == 1);
    for (auto &call : calls)
        assert(call.number != -12); // Successful OOL receive transfers ownership to the caller.
}

void testDestroyMessageResources(mach_msg_descriptor_type_t memoryType)
{
    reset();
    struct Message {
        mach_msg_header_t head;
        mach_msg_body_t body;
        mach_msg_port_descriptor_t port;
        mach_msg_ool_descriptor_t memory;
    } __attribute__((packed, aligned(4))) message = {};
    message.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0) | MACH_MSGH_BITS_COMPLEX;
    message.head.msgh_size = sizeof(message);
    message.head.msgh_remote_port = 55;
    message.body.msgh_descriptor_count = 2;
    message.port.name = 66;
    message.port.disposition = MACH_MSG_TYPE_MOVE_RECEIVE;
    message.port.type = MACH_MSG_PORT_DESCRIPTOR;
    message.memory.address = reinterpret_cast<void *>(0x100000000);
    message.memory.size = 16384;
    message.memory.type = memoryType;
    message.memory.deallocate = true;
    handle = [](const Trap &) -> int64_t { return KERN_SUCCESS; };
    lsb::mach_msg_destroy(&message.head);
    bool sendDestroyed = false, receiveDestroyed = false, memoryDestroyed = false;
    for (auto &call : calls) {
        sendDestroyed |= call.number == -18 && call.args[1] == 55;
        receiveDestroyed |= call.number == -19 && call.args[1] == 66 && call.args[2] == MACH_PORT_RIGHT_RECEIVE;
        memoryDestroyed |= call.number == -12 && call.args[1] == 0x100000000 && call.args[2] == 16384;
    }
    assert(sendDestroyed && receiveDestroyed && memoryDestroyed);
}
} // namespace

namespace lsb {
int64_t machTrap(int64_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                 uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7)
{
    Trap trap = {number, {a0, a1, a2, a3, a4, a5, a6, a7}};
    calls.push_back(trap);
    if (number == -28)
        return kTask;
    if (number == -26)
        return kReply;
    if (number == -19 && a1 == kReply)
        return KERN_SUCCESS;
    return handle ? handle(trap) : KERN_SUCCESS;
}
void *zero(void *address, size_t size) { return std::memset(address, 0, size); }
void *copy(void *destination, const void *source, size_t size) { return std::memcpy(destination, source, size); }
}

int main()
{
    testFastTraps();
    testInterruptedReceiveNeverResends();
    testVMRPCAndKernelError();
    testVMFallbackUses64BitSubsystem();
    testTaskInfoValidation();
    testImageRegistrationAndOwnership();
    testDestroyMessageResources(MACH_MSG_OOL_DESCRIPTOR);
    testDestroyMessageResources(MACH_MSG_OOL_VOLATILE_DESCRIPTOR);
    std::puts("Local Mach callback tests passed");
}
