#include "../Guest/XPCWireCompatibility.h"
#include "../ProcessSupport/PrivateXPC.h"
#include "../ProcessSupport/SingleThreadGuard.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *, mach_port_t *, uint32_t);
enum { NativeMagic = 0x40585043, SimulatorMagic = 0x7870633f, DescriptorCount = 128, PayloadBytes = 70000 };

static int translate(void *message, size_t capacity) {
    return LSBXPCWireTestTranslate(message, capacity, NativeMagic, SimulatorMagic);
}

static void testBounds(void) {
    struct { mach_msg_header_t header; uint32_t words[32]; } message = {0};
    message.header.msgh_size = sizeof(mach_msg_header_t) + 8;
    message.words[0] = NativeMagic; message.words[1] = 5;
    assert(translate(NULL, sizeof(message)) == EPROTO);
    assert(translate(&message, sizeof(mach_msg_header_t) - 1) == EPROTO);
    assert(translate(&message, sizeof(mach_msg_header_t) + 7) == EPROTO);
    assert(!translate(&message, sizeof(message)) && message.words[0] == SimulatorMagic);
    message.words[0] = NativeMagic; message.words[1] = 6;
    assert(translate(&message, sizeof(message)) == EPROTO && message.words[0] == NativeMagic);

    message.header.msgh_bits = MACH_MSGH_BITS_COMPLEX;
    message.header.msgh_size = sizeof(mach_msg_base_t);
    message.words[0] = UINT32_MAX;
    assert(translate(&message, sizeof(message)) == EPROTO);
    message.words[0] = 1;
    assert(translate(&message, sizeof(message)) == EPROTO);
    message.header.msgh_size = sizeof(mach_msg_base_t) + sizeof(mach_msg_ool_descriptor_t);
    mach_msg_ool_descriptor_t descriptor = {.address = (void *)1, .size = 8, .type = MACH_MSG_OOL_DESCRIPTOR};
    memcpy(&message.words[1], &descriptor, sizeof(descriptor));
    assert(translate(&message, sizeof(message)) == EFAULT);
    descriptor.address = (void *)(UINTPTR_MAX - 3);
    memcpy(&message.words[1], &descriptor, sizeof(descriptor));
    assert(translate(&message, sizeof(message)) == EPROTO);
    descriptor.address = NULL;
    memcpy(&message.words[1], &descriptor, sizeof(descriptor));
    assert(translate(&message, sizeof(message)) == EPROTO);

    vm_address_t page = 0;
    assert(!vm_allocate(mach_task_self(), &page, vm_page_size, VM_FLAGS_ANYWHERE));
    uint32_t *header = (uint32_t *)page;
    header[0] = NativeMagic; header[1] = 5;
    descriptor.address = (void *)page;
    memcpy(&message.words[1], &descriptor, sizeof(descriptor));
    assert(!translate(&message, sizeof(message)) && header[0] == SimulatorMagic);
    header[0] = NativeMagic;
    assert(!vm_protect(mach_task_self(), page, vm_page_size, false, VM_PROT_READ));
    assert(translate(&message, sizeof(message)) == EFAULT && header[0] == NativeMagic);
    assert(!vm_deallocate(mach_task_self(), page, vm_page_size));
    descriptor.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
    memcpy(&message.words[1], &descriptor, sizeof(descriptor));
    assert(translate(&message, sizeof(message)) == EPROTO);
}

static xpc_object_t descriptors(int fd) {
    xpc_object_t array = xpc_array_create(NULL, 0);
    for (unsigned i = 0; i < DescriptorCount; ++i) {
        xpc_object_t object = xpc_fd_create(fd);
        assert(object);
        xpc_array_append_value(array, object);
        xpc_release(object);
    }
    return array;
}

static mach_port_t inheritedPort(void) {
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    assert(!mach_ports_lookup(mach_task_self(), &ports, &count) && count && MACH_PORT_VALID(ports[0]));
    mach_port_t port = ports[0];
    for (unsigned i = 1; i < count; ++i) if (MACH_PORT_VALID(ports[i])) mach_port_deallocate(mach_task_self(), ports[i]);
    assert(!vm_deallocate(mach_task_self(), (vm_address_t)ports, count * sizeof(*ports)));
    return port;
}

static void *untranslatedCall(void *pipe) {
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0), reply = NULL;
    assert(!xpc_pipe_routine(pipe, request, &reply));
    assert(reply && xpc_dictionary_get_bool(reply, "ok"));
    xpc_release(reply); xpc_release(request);
    return NULL;
}

static int client(void) {
    alarm(10);
    assert(!LSBSingleThreadGuard());
    testBounds();
    mach_port_t port = inheritedPort();
    char path[] = "/tmp/LiveSimBooter-wire.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    assert(write(fd, "data", 4) == 4 && !lseek(fd, 0, SEEK_SET));
    char *payload = malloc(PayloadBytes + 1);
    assert(payload);
    memset(payload, 'x', PayloadBytes); payload[PayloadBytes] = 0;
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0);
    for (unsigned round = 0; round < 2; ++round) {
        xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0), reply = NULL;
        if (!round) {
            xpc_dictionary_set_string(request, "payload", payload);
            xpc_object_t array = descriptors(fd);
            xpc_dictionary_set_value(request, "fds", array);
            xpc_release(array);
        }
        assert(!LSBXPCWireBegin(port));
        assert(LSBXPCWireBegin(port) == EBUSY);
        assert(!xpc_pipe_routine(pipe, request, &reply));
        LSBXPCWireEnd();
        assert(reply && !strcmp(xpc_dictionary_get_string(reply, "payload"), payload));
        xpc_object_t array = xpc_dictionary_get_value(reply, "fds");
        assert(xpc_array_get_count(array) == DescriptorCount);
        for (unsigned i = 0; i < DescriptorCount; ++i) {
            int duplicate = xpc_fd_dup(xpc_array_get_value(array, i));
            assert(duplicate >= 0 && lseek(duplicate, 0, SEEK_CUR) == 1);
            close(duplicate);
        }
        xpc_release(reply); xpc_release(request);
    }
    assert(lseek(fd, 0, SEEK_CUR) == 1);
    LSBXPCWireStatistics stats = LSBXPCWireTestStatistics();
    fprintf(stderr, "XPC wire large graph: sent=%u/%u received=%u/%u retries=%u (inline/OOL)\n",
            stats.sentInline, stats.sentOOL, stats.receivedInline, stats.receivedOOL, stats.receiveRetries);
    assert(stats.sentInline && stats.sentOOL && stats.receivedOOL && stats.receiveRetries);
    mach_port_t unrelated;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &unrelated));
    assert(!mach_port_insert_right(mach_task_self(), unrelated, unrelated, MACH_MSG_TYPE_MAKE_SEND));
    assert(!LSBXPCWireBegin(unrelated));
    untranslatedCall(pipe); // An active context must not alter another destination on the same thread.
    LSBXPCWireEnd();
    assert(!mach_port_deallocate(mach_task_self(), unrelated));
    assert(!mach_port_mod_refs(mach_task_self(), unrelated, MACH_PORT_RIGHT_RECEIVE, -1));
    assert(!LSBXPCWireBegin(port));
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, untranslatedCall, pipe));
    assert(!pthread_join(worker, NULL)); // A different thread must remain native even while this thread's broker context is active.
    LSBXPCWireEnd();
    LSBXPCWireStatistics after = LSBXPCWireTestStatistics();
    assert(!memcmp(&stats, &after, sizeof(stats)));
    xpc_release(pipe); close(fd); free(payload);
    assert(!mach_port_deallocate(mach_task_self(), port));
    int guard = EBUSY;
    for (unsigned i = 0; i < 100 && guard == EBUSY; ++i) { usleep(1000); guard = LSBSingleThreadGuard(); }
    assert(!guard);
    return 0;
}

static int host(const char *executable) {
    alarm(15);
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    assert(!posix_spawnattr_set_registered_ports_np(&attributes, &port, 1));
    char *argv[] = {(char *)executable, "--client", NULL};
    pid_t child;
    assert(!posix_spawn(&child, executable, NULL, &attributes, argv, environ));
    posix_spawnattr_destroy(&attributes);
    xpc_object_t saved = NULL;
    for (unsigned round = 0; round < 4; ++round) {
        xpc_object_t request = NULL;
        assert(!xpc_pipe_receive(port, &request, 0) && request);
        audit_token_t audit;
        xpc_dictionary_get_audit_token(request, &audit);
        assert((pid_t)audit.val[5] == child);
        if (!round) {
            saved = xpc_retain(request);
            int fd = xpc_fd_dup(xpc_array_get_value(xpc_dictionary_get_value(request, "fds"), 0));
            char byte;
            assert(fd >= 0 && read(fd, &byte, 1) == 1 && byte == 'd');
            close(fd);
        }
        xpc_object_t reply = xpc_dictionary_create_reply(request);
        assert(reply);
        if (round < 2) {
            xpc_dictionary_set_string(reply, "payload", xpc_dictionary_get_string(saved, "payload"));
            xpc_dictionary_set_value(reply, "fds", xpc_dictionary_get_value(saved, "fds"));
        } else xpc_dictionary_set_bool(reply, "ok", true);
        assert(!xpc_pipe_routine_reply(reply));
        xpc_release(reply); xpc_release(request);
    }
    xpc_release(saved);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!mach_port_deallocate(mach_task_self(), port));
    assert(!mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1));
    puts("XPC wire: inline/OOL translation, 128 FDs, shared offsets, retries, malformed bounds, and thread/destination isolation passed");
    return 0;
}

int main(int argc, char *argv[]) { return argc > 1 ? client() : host(argv[0]); }
