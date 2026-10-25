#include "../Guest/ProcessIdentity.h"
#include <assert.h>
#include <mach/mach.h>
#include <os/log.h>
#include <stdio.h>
#include <unistd.h>
#include <xpc/xpc.h>
extern xpc_object_t xpc_copy_entitlements_for_self(void);
int main(void) {
    assert(getpid() == 2147483000 && LSBGuestPID() == 2147483000 && LSBNativePID() != 2147483000);
    audit_token_t token = {{0}};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    assert(token.val[5] == (unsigned)LSBNativePID());
    mach_port_t taskName = MACH_PORT_NULL;
    int nativePID = 0;
    assert(task_name_for_pid(mach_task_self(), getpid(), &taskName) == KERN_SUCCESS && MACH_PORT_VALID(taskName));
    assert(pid_for_task(taskName, &nativePID) == KERN_SUCCESS && nativePID == LSBNativePID());
    assert(mach_port_deallocate(mach_task_self(), taskName) == KERN_SUCCESS);
    xpc_object_t entitlements = xpc_copy_entitlements_for_self();
    if (entitlements) xpc_release(entitlements);
    os_log_t log = os_log_create("com.kdt.LiveSimBooter.identity-smoke", "self");
    assert(log);
    os_release(log);
    puts("Identity smoke: guest getpid, native task-name right, unchanged audit token, libxpc self lookup and firehose passed");
}
