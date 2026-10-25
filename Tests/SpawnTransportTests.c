#include "../ProcessSupport/SpawnTransport.h"
#include "../ProcessSupport/PrivateXPC.h"
#if defined(LSB_XPC_WIRE_TESTING)
#include "../Guest/XPCWireCompatibility.h"
#endif

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

int LSBTestCopyBrokerPort(mach_port_t *port) {
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    if (mach_ports_lookup(mach_task_self(), &ports, &count)) return EIO;
    *port = count ? ports[0] : MACH_PORT_NULL;
    for (unsigned i = 1; i < count; ++i) if (MACH_PORT_VALID(ports[i])) mach_port_deallocate(mach_task_self(), ports[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)ports, count * sizeof(*ports));
    return MACH_PORT_VALID(*port) ? 0 : ENOENT;
}

static LSBSpawnPlan makePlan(void) {
    LSBSpawnPlan plan = {.version = LSB_PLAN_VERSION, .argc = 2, .envc = 1, .fdCount = 1, .creationMask = 022};
    assert(!LSBPlanAddString(&plan, "/bin/df", &plan.path));
    plan.argv[0] = plan.path;
    assert(!LSBPlanAddString(&plan, "-h", &plan.argv[1]));
    assert(!LSBPlanAddString(&plan, "/", &plan.cwd));
    assert(!LSBPlanAddString(&plan, "PATH=/bin:/usr/bin", &plan.envp[0]));
    plan.fds[0].number = 7;
    plan.fds[0].flags = FD_CLOEXEC;
    assert(!LSBValidateSpawnPlan(&plan));
    return plan;
}

static void testMalformedRequest(const LSBSpawnPlan *plan, unsigned variant) {
    mach_port_t port;
    assert(!LSBTestCopyBrokerPort(&port));
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0);
    assert(pipe);
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(request, "version", 1);
    xpc_dictionary_set_uint64(request, "kind", LSB_REQUEST_SPAWN);
    xpc_dictionary_set_uint64(request, "request-id", 0);
    xpc_dictionary_set_int64(request, "error", 0);
    LSBSpawnPlan malformed = *plan;
    if (variant == 4) { malformed.fdCount = 2; malformed.fds[1].number = 8; }
    xpc_dictionary_set_data(request, "plan", &malformed, variant == 1 ? sizeof(*plan) - 1 : sizeof(*plan));
    xpc_object_t fds = xpc_array_create(NULL, 0);
    if (variant == 4) {
        xpc_object_t descriptor = xpc_fd_create(STDERR_FILENO);
        assert(descriptor);
        xpc_array_append_value(fds, descriptor);
        xpc_release(descriptor);
    }
    if (variant != 3) xpc_array_set_string(fds, XPC_ARRAY_APPEND, "not a descriptor");
    xpc_dictionary_set_value(request, "fds", fds);
    xpc_release(fds);
    if (variant == 0) xpc_dictionary_set_string(request, "kind", "not an integer");
    xpc_object_t reply = NULL;
    assert(!xpc_pipe_routine(pipe, request, &reply));
    assert(reply && xpc_dictionary_get_int64(reply, "error") == EPROTO);
    xpc_release(reply);
    xpc_release(request);
    xpc_release(pipe);
    mach_port_deallocate(mach_task_self(), port);
    assert(!LSBRequireSingleThread());
}

static void *blockedWorker(void *argument) {
    char byte;
    assert(read(*(int *)argument, &byte, 1) == 1);
    return NULL;
}

#ifdef LSB_GUEST_RUNTIME
static mach_port_t testGuestPort(void) {
    mach_port_t base = MACH_PORT_NULL, copy = MACH_PORT_NULL;
    assert(!LSBTestCopyBrokerPort(&base));
    assert(LSBTransportCopyBrokerPort(NULL) == EINVAL);
    unsetenv(LSB_BROKER_PORT_ENV);
    assert(LSBTransportCopyBrokerPort(&copy) == ENOENT && copy == MACH_PORT_NULL);
    const char *invalid[] = {"", "0", "-1", "+1", " 1", "1x", "4294967295", "4294967296", "100000000000000000000000"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!setenv(LSB_BROKER_PORT_ENV, invalid[i], 1));
        assert(LSBTransportCopyBrokerPort(&copy) == EINVAL && copy == MACH_PORT_NULL);
    }
    mach_port_t receiveOnly;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &receiveOnly));
    char value[16];
    snprintf(value, sizeof(value), "%u", receiveOnly);
    assert(!setenv(LSB_BROKER_PORT_ENV, value, 1));
    assert(LSBTransportCopyBrokerPort(&copy) == EACCES && copy == MACH_PORT_NULL);
    assert(!mach_port_mod_refs(mach_task_self(), receiveOnly, MACH_PORT_RIGHT_RECEIVE, -1));
    snprintf(value, sizeof(value), "%u", base);
    assert(!setenv(LSB_BROKER_PORT_ENV, value, 1));
    mach_port_urefs_t before = 0, after = 0;
    assert(!mach_port_get_refs(mach_task_self(), base, MACH_PORT_RIGHT_SEND, &before));
    assert(!LSBTransportCopyBrokerPort(&copy) && copy == base);
    assert(!mach_port_get_refs(mach_task_self(), base, MACH_PORT_RIGHT_SEND, &after) && after == before + 1);
    assert(!mach_port_deallocate(mach_task_self(), copy));
    assert(!mach_port_get_refs(mach_task_self(), base, MACH_PORT_RIGHT_SEND, &after) && after == before);
    return base;
}
#endif

static int client(void) {
    assert(!LSBRequireSingleThread());
#ifdef LSB_GUEST_RUNTIME
    mach_port_t brokerPort = testGuestPort();
#endif
    LSBSpawnPlan plan = makePlan();
    char path[] = "/tmp/LiveSimBooter-transport.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    unlink(path);
    assert(write(fd, "abcdef", 6) == 6 && lseek(fd, 0, SEEK_SET) == 0);
    int fds[LSB_MAX_FDS];
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) fds[i] = -1;
    fds[0] = fd;
    pid_t pid = 0;
    assert(!LSBTransportSpawn(&plan, fds, &pid) && pid == 4242);
    assert(lseek(fd, 0, SEEK_CUR) == 1 && fcntl(fd, F_GETFD) >= 0);
    assert(!LSBRequireSingleThread());

    LSBBootstrapReply reply;
    int received[LSB_MAX_FDS];
    assert(!LSBTransportCheckIn(&reply, received));
    assert(reply.requestID == 77 && !reply.error && !strcmp(reply.runtimeRoot, "/tmp/RuntimeRoot"));
    assert(!memcmp(&reply.plan, &plan, sizeof(plan)) && received[0] >= 0);
    for (unsigned i = 1; i < LSB_MAX_FDS; ++i) assert(received[i] == -1);
    char byte = 0;
    assert(read(received[0], &byte, 1) == 1 && byte == 'b' && lseek(fd, 0, SEEK_CUR) == 2);
    close(received[0]);
    assert(!LSBTransportReady(reply.requestID, 0));
    assert(!LSBRequireSingleThread());
    assert(LSBTransportReady(0, 0) == EINVAL);
    plan.version = 0;
    assert(LSBTransportSpawn(&plan, fds, &pid) == EINVAL && pid == 0 && fcntl(fd, F_GETFD) >= 0);
    plan.version = LSB_PLAN_VERSION;
    assert(LSBTransportSpawn(&plan, fds, &pid) == EACCES && pid == 0);
    assert(LSBTransportSpawn(&plan, fds, &pid) == EPROTO && pid == 0);
    for (unsigned variant = 0; variant < 5; ++variant) testMalformedRequest(&plan, variant);
    assert(LSBTransportReady(77, ENOEXEC) == ENOEXEC);
    assert(!LSBRequireSingleThread());
    close(fd);
    int synchronization[2];
    assert(!pipe(synchronization));
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, blockedWorker, &synchronization[0]));
    assert(LSBRequireSingleThread() == EBUSY);
    assert(write(synchronization[1], "x", 1) == 1 && !pthread_join(worker, NULL));
    close(synchronization[0]);
    close(synchronization[1]);
    // pthread_join can finish just before the kernel removes the exiting thread from task_threads.
    int guard = EBUSY;
    for (unsigned i = 0; i < 100 && guard == EBUSY; ++i) { usleep(1000); guard = LSBRequireSingleThread(); }
    assert(!guard);
#ifdef LSB_GUEST_RUNTIME
    assert(!mach_port_deallocate(mach_task_self(), brokerPort));
#endif
#if defined(LSB_XPC_WIRE_TESTING)
    LSBXPCWireStatistics statistics = LSBXPCWireTestStatistics();
    fprintf(stderr, "XPC wire adapter: sent inline=%u OOL=%u, received inline=%u OOL=%u, receive retries=%u\n",
            statistics.sentInline, statistics.sentOOL, statistics.receivedInline, statistics.receivedOOL, statistics.receiveRetries);
    assert(statistics.sentInline && statistics.receivedInline);
#endif
    puts("SpawnTransport child: payload, shared FD, error, malformed-message and single-thread checks passed");
    return 0;
}

static void *replyWorker(void *argument) {
    LSBReceivedMessage *message = argument;
    assert(!LSBTransportReply(message, 0, 4242, 77, NULL, NULL, NULL));
    return NULL;
}

static unsigned openDescriptorCount(void) {
    unsigned count = 0;
    for (int fd = 0; fd < 256; ++fd) if (fcntl(fd, F_GETFD) >= 0) ++count;
    return count;
}

static int host(const char *executable) {
    mach_port_t port;
    assert(!mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
    assert(!mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
    posix_spawnattr_t attributes;
    assert(!posix_spawnattr_init(&attributes));
    assert(!posix_spawnattr_set_registered_ports_np(&attributes, &port, 1));
    char *arguments[] = {(char *)executable, "--client", NULL};
    pid_t child;
    assert(!posix_spawn(&child, executable, NULL, &attributes, arguments, environ));
    posix_spawnattr_destroy(&attributes);

    LSBReceivedMessage pending;
    assert(!LSBTransportReceive(port, &pending));
    assert(pending.kind == LSB_REQUEST_SPAWN && pending.audit.val[5] == (unsigned)child);
    assert(pending.plan.fdCount == 1 && pending.fds[0] >= 0);
    char byte = 0;
    assert(read(pending.fds[0], &byte, 1) == 1 && byte == 'a');
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, replyWorker, &pending));
    assert(!pthread_join(worker, NULL));
    assert(pending.object == NULL && fcntl(pending.fds[0], F_GETFD) >= 0);
    LSBReceivedMessage message;
    assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_CHECKIN);
    assert(message.audit.val[5] == (unsigned)child);
    assert(!LSBTransportReply(&message, 0, child, 77, "/tmp/RuntimeRoot", &pending.plan, pending.fds));
    LSBTransportDisposeMessage(&message);
    LSBTransportDisposeMessage(&pending);
    assert(pending.fds[0] == -1);
    assert(!LSBTransportReceive(port, &message) && message.kind == LSB_REQUEST_READY && message.requestID == 77 && !message.error);
    assert(!LSBTransportReply(&message, 0, child, 77, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);

    assert(!LSBTransportReceive(port, &message));
    assert(!LSBTransportReply(&message, EACCES, 0, 0, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    assert(!LSBTransportReceive(port, &message));
    xpc_object_t invalid = xpc_dictionary_create_reply((xpc_object_t)message.object);
    assert(invalid);
    xpc_dictionary_set_string(invalid, "version", "not an integer");
    assert(!xpc_pipe_routine_reply(invalid));
    xpc_release(invalid);
    LSBTransportDisposeMessage(&message);
    for (unsigned variant = 0; variant < 5; ++variant) {
        unsigned before = openDescriptorCount();
        assert(LSBTransportReceive(port, &message) == EPROTO && message.object == NULL);
        for (unsigned i = 0; i < LSB_MAX_FDS; ++i) assert(message.fds[i] == -1);
        assert(openDescriptorCount() == before);
    }
    assert(!LSBTransportReceive(port, &message) && message.error == ENOEXEC);
    assert(!LSBTransportReply(&message, ENOEXEC, 0, 77, NULL, NULL, NULL));
    LSBTransportDisposeMessage(&message);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    mach_port_deallocate(mach_task_self(), port);
    mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE, -1);
    puts("SpawnTransport parent: retained asynchronous reply ownership and audit identity checks passed");
    return 0;
}

int main(int argc, char **argv) {
    // Bound a failed regression test without adding a signal handler to the production transport.
    alarm(20);
    return argc == 2 && !strcmp(argv[1], "--client") ? client() : host(argv[0]);
}
