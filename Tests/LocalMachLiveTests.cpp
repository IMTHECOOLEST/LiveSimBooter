#include "../LiveSimBooter/LocalMach.h"

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>

// Safe host checks: operate only on this test process's allocated VM/ports and query existing task state.
// Never detach a cache, register dyld images, or alter host dyld process information.
int main()
{
    auto task = lsb::task_self_trap();
    assert(task == mach_task_self());

    mach_timebase_info_data_t localTimebase, systemTimebase;
    assert(lsb::mach_timebase_info(&localTimebase) == KERN_SUCCESS);
    assert(::mach_timebase_info(&systemTimebase) == KERN_SUCCESS);
    assert(localTimebase.numer == systemTimebase.numer && localTimebase.denom == systemTimebase.denom);
    auto first = ::mach_absolute_time();
    auto measured = lsb::mach_absolute_time();
    auto last = ::mach_absolute_time();
    assert(first <= measured && measured <= last);

    constexpr vm_size_t allocationSize = 16384;
    vm_address_t source = 0, destination = 0;
    assert(lsb::vm_allocate(task, &source, allocationSize, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    assert(lsb::vm_allocate(task, &destination, allocationSize, VM_FLAGS_ANYWHERE) == KERN_SUCCESS);
    std::memset(reinterpret_cast<void *>(source), 0x5a, allocationSize);
    assert(lsb::vm_copy(task, source, allocationSize, destination) == KERN_SUCCESS);
    assert(std::memcmp(reinterpret_cast<void *>(source), reinterpret_cast<void *>(destination), allocationSize) == 0);
    assert(lsb::vm_protect(task, destination, allocationSize, false, VM_PROT_READ) == KERN_SUCCESS);
    assert(lsb::vm_deallocate(task, source, allocationSize) == KERN_SUCCESS);
    assert(lsb::vm_deallocate(task, destination, allocationSize) == KERN_SUCCESS);

    task_basic_info_64_data_t taskInfo;
    mach_msg_type_number_t count = TASK_BASIC_INFO_64_COUNT;
    assert(lsb::task_info(task, TASK_BASIC_INFO_64, reinterpret_cast<task_info_t>(&taskInfo), &count) == KERN_SUCCESS);
    assert(count == TASK_BASIC_INFO_64_COUNT);
    auto thread = lsb::mach_thread_self();
    thread_basic_info_data_t threadInfo;
    count = THREAD_BASIC_INFO_COUNT;
    assert(lsb::thread_info(thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&threadInfo), &count) == KERN_SUCCESS);
    assert(count == THREAD_BASIC_INFO_COUNT);
    assert(lsb::mach_port_deallocate(task, thread) == KERN_SUCCESS);

    mach_port_t port;
    assert(lsb::mach_port_allocate(task, MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS);
    assert(lsb::mach_port_insert_right(task, port, port, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    struct Message {
        mach_msg_header_t header;
        uint64_t payload;
        mach_msg_trailer_t trailer;
    } message = {};
    message.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    message.header.msgh_size = offsetof(Message, trailer);
    message.header.msgh_remote_port = port;
    message.header.msgh_id = 12345;
    message.payload = UINT64_C(0x123456789abcdef0);
    auto messageOptions = MACH_SEND_MSG | MACH_RCV_MSG | MACH_SEND_TIMEOUT | MACH_RCV_TIMEOUT;
    assert(lsb::mach_msg(&message.header, messageOptions, message.header.msgh_size, sizeof(message), port, 1000, 0) == MACH_MSG_SUCCESS);
    assert(message.header.msgh_id == 12345 && message.payload == UINT64_C(0x123456789abcdef0));
    lsb::mach_msg_destroy(&message.header);
    assert(lsb::mach_port_deallocate(task, port) == KERN_SUCCESS);
    assert(lsb::mach_port_destruct(task, port, 0, 0) == KERN_SUCCESS);

    mach_port_options_t options = {};
    assert(lsb::mach_port_construct(task, &options, 0, &port) == KERN_SUCCESS);
    assert(lsb::mach_port_destruct(task, port, 0, 0) == KERN_SUCCESS);

    dyld_kernel_image_info_array_t images;
    count = 0;
    auto result = lsb::task_get_dyld_image_infos(task, &images, &count);
    assert(result == KERN_SUCCESS || result == KERN_NOT_SUPPORTED);
    if (result == KERN_SUCCESS && count)
        assert(lsb::vm_deallocate(task, reinterpret_cast<vm_address_t>(images), count * sizeof(*images)) == KERN_SUCCESS);
    dyld_kernel_process_info_t process;
    result = lsb::task_register_dyld_get_process_state(task, &process);
    assert(result == KERN_SUCCESS || result == KERN_NOT_SUPPORTED);

    std::puts("Live host Mach callback tests passed (no cache detach or dyld mutation)");
}
