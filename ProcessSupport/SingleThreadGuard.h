#pragma once

#include <errno.h>
#include <mach/mach.h>

// This is a fail-closed snapshot, not a way to stop arbitrary asynchronous work. Call only after releasing native IPC state.
static inline int LSBSingleThreadGuard(void) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        thread_act_array_t threads = NULL;
        mach_msg_type_number_t count = 0;
        kern_return_t result = task_threads(mach_task_self(), &threads, &count);
        if (result) return EIO;
        mach_port_t self = mach_thread_self();
        int single = count == 1 && threads[0] == self;
        for (unsigned i = 0; i < count; ++i) mach_port_deallocate(mach_task_self(), threads[i]);
        mach_port_deallocate(mach_task_self(), self);
        vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(*threads));
        if (!single) return EBUSY;
    }
    return 0;
}
