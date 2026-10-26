#import "../Host/IOSurfaceServer.m"
#include <assert.h>
#include <dlfcn.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

extern mach_port_t xpc_dictionary_copy_mach_send(xpc_object_t dictionary, const char *key);
extern xpc_endpoint_t xpc_endpoint_create_mach_port_4sim(mach_port_t port);
extern int posix_spawnattr_set_registered_ports_np(posix_spawnattr_t *attributes, mach_port_t *ports, uint32_t count);
extern char **environ;

@interface LSBIOSurfaceServer (Testing)
- (xpc_endpoint_t)testEndpoint;
@end
@implementation LSBIOSurfaceServer (Testing)
- (xpc_endpoint_t)testEndpoint { return xpc_endpoint_create(_listener); }
@end

static audit_token_t ownAudit(void) {
    audit_token_t token;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&token, &count));
    return token;
}

static xpc_connection_t clientForEndpoint(xpc_endpoint_t endpoint) {
    xpc_connection_t client = xpc_connection_create_from_endpoint(endpoint);
    xpc_connection_set_event_handler(client, ^(xpc_object_t ignored) { (void)ignored; });
    xpc_connection_enable_sim2host_4sim(client);
    xpc_connection_activate(client);
    return client;
}

static xpc_connection_t clientFor(LSBIOSurfaceServer *server) { return clientForEndpoint([server testEndpoint]); }

static xpc_object_t sendRequest(xpc_connection_t client, xpc_object_t message) {
    dispatch_semaphore_t complete = dispatch_semaphore_create(0);
    __block xpc_object_t result;
    xpc_connection_send_message_with_reply(client, message, dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^(xpc_object_t reply) {
        result = reply;
        dispatch_semaphore_signal(complete);
    });
    assert(!dispatch_semaphore_wait(complete, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC)));
    return result;
}

static xpc_object_t request(xpc_connection_t client, uint64_t method) {
    xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(message, "Method", method);
    return sendRequest(client, message);
}

static xpc_object_t surfaceRequest(xpc_connection_t client, uint64_t method, uint32_t surfaceID) {
    xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(message, "Method", method);
    xpc_dictionary_set_uint64(message, "SurfaceID", surfaceID);
    xpc_dictionary_set_uint64(message, "Options", 0);
    xpc_object_t reply = sendRequest(client, message);
    assert(xpc_get_type(reply) == XPC_TYPE_DICTIONARY);
    assert(!xpc_dictionary_get_uint64(reply, "ErrorCode"));
    return reply;
}

static uint32_t testPixel(unsigned x, unsigned y) { return 0xff000000u | (x << 16) | (y << 8) | (x ^ y); }

static void testSurfaceAllocationWithAction(xpc_connection_t client, void (^whileSurfaceLive)(void)) {
    // The iOS 26 guest serializes CF dictionaries with IOCFSerialize, through this same exported IOSurface helper.
    void (*serialize)(xpc_object_t, const char *, CFTypeRef) = dlsym(RTLD_DEFAULT, "_ioSurfaceSerializeToXPCDictionaryWithKey");
    assert(serialize);
    NSDictionary *properties = @{
        (__bridge NSString *)kIOSurfaceWidth: @64, (__bridge NSString *)kIOSurfaceHeight: @32,
        (__bridge NSString *)kIOSurfaceBytesPerElement: @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow: @(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, 256)),
        (__bridge NSString *)kIOSurfacePixelFormat: @0x42475241
    };
    xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_uint64(message, "Method", 2);
    serialize(message, "PropertiesDictionary", (__bridge CFDictionaryRef)properties);
    xpc_object_t reply = sendRequest(client, message);
    if (xpc_get_type(reply) != XPC_TYPE_DICTIONARY || xpc_dictionary_get_uint64(reply, "ErrorCode")) {
        char *description = xpc_copy_description(reply);
        fprintf(stderr, "IOSurface allocation reply: %s\n", description);
        free(description);
        description = xpc_copy_description(message);
        fprintf(stderr, "IOSurface allocation request: %s\n", description);
        free(description);
    }
    assert(xpc_get_type(reply) == XPC_TYPE_DICTIONARY && !xpc_dictionary_get_uint64(reply, "ErrorCode"));
    size_t stateSize = 0;
    const void *stateBytes = xpc_dictionary_get_data(reply, "SurfaceState", &stateSize);
    assert(stateBytes && stateSize >= 72);
    uint32_t state[18];
    memcpy(state, stateBytes, sizeof(state));
    // Guest _ioSurfaceSyncState copies 72 bytes: ID@4, allocation@8, width@12, height@16, stride@20, base offset@24.
    assert(state[1] && state[2] >= 8192 && state[3] == 64 && state[4] == 32 && state[5] >= 256 && state[7] == 0x42475241);
    xpc_object_t memory = xpc_dictionary_get_value(reply, "MemoryObject");
    assert(memory && xpc_get_type(memory) == XPC_TYPE_SHMEM);
    void *mapping = NULL;
    size_t mappingSize = xpc_shmem_map(memory, &mapping);
    assert(mapping && mappingSize >= state[6] && mappingSize - state[6] >= (size_t)state[5] * 32);

    surfaceRequest(client, 4, state[1]); // The guest's normal IOSurfaceLock request.
    uint8_t *pixels = (uint8_t *)mapping + state[6];
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 64; ++x) {
        uint32_t pixel = testPixel(x, y);
        memcpy(pixels + y * state[5] + x * 4, &pixel, sizeof(pixel));
    }
    surfaceRequest(client, 5, state[1]);
    if (whileSurfaceLive) whileSurfaceLive();

    xpc_object_t exported = surfaceRequest(client, 14, state[1]);
    mach_port_t port = xpc_dictionary_copy_mach_send(exported, "MachPort");
    assert(MACH_PORT_VALID(port));
    IOSurfaceRef surface = IOSurfaceLookupFromMachPort(port);
    assert(surface && IOSurfaceGetID(surface) == state[1]);
    assert(IOSurfaceGetWidth(surface) == 64 && IOSurfaceGetHeight(surface) == 32 && IOSurfaceGetPixelFormat(surface) == 0x42475241);
    assert(!IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL));
    const uint8_t *nativePixels = IOSurfaceGetBaseAddress(surface);
    assert(nativePixels && IOSurfaceGetBytesPerRow(surface) == state[5]);
    for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 64; ++x) {
        uint32_t pixel;
        memcpy(&pixel, nativePixels + y * state[5] + x * 4, sizeof(pixel));
        assert(pixel == testPixel(x, y));
    }
    assert(!IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL));
    CFRelease(surface);
    assert(!mach_port_deallocate(mach_task_self(), port));
    assert(!munmap(mapping, mappingSize));
    surfaceRequest(client, 3, state[1]); // Balance the remote create independently of the exported Mach right.
}

static void testSurfaceAllocation(xpc_connection_t client) { testSurfaceAllocationWithAction(client, nil); }

static void awaitDisconnected(xpc_connection_t client) {
    // Cancellation is asynchronous; an already queued native framework reply may arrive before connection invalidation.
    for (unsigned i = 0; i < 100; ++i) {
        if (xpc_get_type(request(client, 0)) == XPC_TYPE_ERROR) return;
        usleep(1000);
    }
    assert(!"IOSurface connection did not invalidate");
}

// A spawned copy receives only this public listener's send right. No process needs registration or an identity allowlist.
// Commands travel over ordinary pipes, and the service receives each client's actual kernel audit trailer.
typedef struct { uint32_t operation, index; } PeerCommand;
typedef struct { pid_t pid; int input, output; audit_token_t audit; } TestPeer;
enum { PeerCreate = 1, PeerRequest, PeerAllocate, PeerDisconnected, PeerCancel, PeerQuit, TestClientCount = LSBMaximumIOSurfacePeers + 1 };

static void writeBytes(int fd, const void *bytes, size_t size) {
    const uint8_t *cursor = bytes;
    while (size) {
        ssize_t count = write(fd, cursor, size);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        cursor += count; size -= (size_t)count;
    }
}

static void readBytes(int fd, void *bytes, size_t size) {
    uint8_t *cursor = bytes;
    while (size) {
        ssize_t count = read(fd, cursor, size);
        if (count < 0 && errno == EINTR) continue;
        assert(count > 0);
        cursor += count; size -= (size_t)count;
    }
}

static int runPeer(int input, int output) {
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t portCount = 0;
    assert(!mach_ports_lookup(mach_task_self(), &ports, &portCount));
    assert(portCount >= 1 && MACH_PORT_VALID(ports[0]));
    xpc_endpoint_t endpoint = xpc_endpoint_create_mach_port_4sim(ports[0]);
    assert(endpoint);
    // The endpoint adopts the supplied send right, just like the simulator bootstrap endpoint adapter.
    for (mach_msg_type_number_t i = 1; i < portCount; ++i)
        if (MACH_PORT_VALID(ports[i])) assert(!mach_port_deallocate(mach_task_self(), ports[i]));
    assert(!vm_deallocate(mach_task_self(), (vm_address_t)ports, portCount * sizeof(*ports)));
    audit_token_t token = ownAudit();
    writeBytes(output, &token, sizeof(token));
    xpc_connection_t clients[TestClientCount] = {nil};
    for (;;) @autoreleasepool {
        PeerCommand command;
        readBytes(input, &command, sizeof(command));
        assert(command.index < TestClientCount);
        uint32_t result = 1;
        if (command.operation == PeerCreate) {
            if (clients[command.index]) xpc_connection_cancel(clients[command.index]);
            clients[command.index] = clientForEndpoint(endpoint);
            result = xpc_get_type(request(clients[command.index], 0)) == XPC_TYPE_DICTIONARY;
        } else if (command.operation == PeerRequest) {
            assert(clients[command.index]);
            result = xpc_get_type(request(clients[command.index], 0)) == XPC_TYPE_DICTIONARY;
        } else if (command.operation == PeerAllocate) {
            assert(clients[command.index]);
            testSurfaceAllocation(clients[command.index]);
        } else if (command.operation == PeerDisconnected) {
            assert(clients[command.index]);
            awaitDisconnected(clients[command.index]);
        } else if (command.operation == PeerCancel) {
            assert(clients[command.index]);
            xpc_connection_cancel(clients[command.index]);
            clients[command.index] = nil;
        } else assert(command.operation == PeerQuit);
        writeBytes(output, &result, sizeof(result));
        if (command.operation == PeerQuit) break;
    }
    for (unsigned i = 0; i < TestClientCount; ++i) if (clients[i]) xpc_connection_cancel(clients[i]);
    close(input); close(output);
    return 0;
}

static TestPeer startPeer(const char *executable, LSBIOSurfaceServer *server) {
    int input[2], output[2];
    assert(!pipe(input) && !pipe(output));
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    assert(!posix_spawn_file_actions_init(&actions));
    assert(!posix_spawnattr_init(&attributes));
    assert(!posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT));
    assert(!posix_spawn_file_actions_adddup2(&actions, input[0], 100));
    assert(!posix_spawn_file_actions_adddup2(&actions, output[1], 101));
    assert(!posix_spawn_file_actions_addclose(&actions, input[0]));
    assert(!posix_spawn_file_actions_addclose(&actions, input[1]));
    assert(!posix_spawn_file_actions_addclose(&actions, output[0]));
    assert(!posix_spawn_file_actions_addclose(&actions, output[1]));
    mach_port_t port = server.registrationPort;
    assert(!posix_spawnattr_set_registered_ports_np(&attributes, &port, 1));
    char *arguments[] = {(char *)executable, "--public-client", NULL};
    TestPeer peer = {.input = input[1], .output = output[0]};
    assert(!posix_spawn(&peer.pid, executable, &actions, &attributes, arguments, environ));
    assert(!posix_spawn_file_actions_destroy(&actions));
    assert(!posix_spawnattr_destroy(&attributes));
    close(input[0]); close(output[1]);
    readBytes(peer.output, &peer.audit, sizeof(peer.audit));
    assert(peer.audit.val[5] == (uint32_t)peer.pid);
    return peer;
}

static uint32_t peerCommand(const TestPeer *peer, uint32_t operation, uint32_t index) {
    PeerCommand command = {operation, index};
    writeBytes(peer->input, &command, sizeof(command));
    uint32_t result;
    readBytes(peer->output, &result, sizeof(result));
    return result;
}

static void stopPeer(TestPeer *peer) {
    assert(peerCommand(peer, PeerQuit, 0));
    close(peer->input); close(peer->output);
    int status = -1;
    assert(waitpid(peer->pid, &status, 0) == peer->pid && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void testPublicClients(const char *executable, LSBIOSurfaceServer *server) {
    TestPeer second = startPeer(executable, server), third = startPeer(executable, server);
    assert(second.pid != third.pid && second.pid != getpid() && third.pid != getpid());
    xpc_connection_t clients[TestClientCount] = {nil};
    // All three real processes connect without any broker authorization, including the former third-owner rejection case.
    clients[0] = clientFor(server);
    assert(xpc_get_type(request(clients[0], 0)) == XPC_TYPE_DICTIONARY);
    assert(peerCommand(&second, PeerCreate, 0));
    assert(peerCommand(&third, PeerCreate, 0));
    assert(server.connectedClientPID == third.pid);
    assert(peerCommand(&second, PeerAllocate, 0));
    assert(peerCommand(&third, PeerAllocate, 0));
    testSurfaceAllocationWithAction(clients[0], ^{
        assert(peerCommand(&second, PeerAllocate, 0));
        assert(peerCommand(&third, PeerRequest, 0));
        // Another client's surface allocation leaves this client's live surface and shared pixels intact.
    });

    for (unsigned i = 1; i < LSBMaximumIOSurfacePeers - 2; ++i) {
        clients[i] = clientFor(server);
        assert(xpc_get_type(request(clients[i], 0)) == XPC_TYPE_DICTIONARY);
    }
    clients[LSBMaximumIOSurfacePeers - 2] = clientFor(server);
    assert(xpc_get_type(request(clients[LSBMaximumIOSurfacePeers - 2], 0)) == XPC_TYPE_ERROR);
    assert(!peerCommand(&second, PeerCreate, 1));
    assert(!peerCommand(&third, PeerCreate, 1));
    assert(peerCommand(&second, PeerAllocate, 0));
    testSurfaceAllocation(clients[0]);

    assert(peerCommand(&second, PeerCancel, 0));
    BOOL reconnected = NO;
    for (unsigned attempt = 0; attempt < 100 && !reconnected; ++attempt) {
        reconnected = peerCommand(&second, PeerCreate, 1);
        if (!reconnected) usleep(1000);
    }
    assert(reconnected); // A disconnected peer frees capacity; the weak collection must not retain it indefinitely.
    assert(peerCommand(&second, PeerAllocate, 1));
    assert(peerCommand(&third, PeerRequest, 0));

    [server close];
    assert(!server.registrationPort && !server.connectedClientPID);
    awaitDisconnected(clients[0]);
    assert(peerCommand(&second, PeerDisconnected, 1));
    assert(peerCommand(&third, PeerDisconnected, 0));
    assert(!peerCommand(&second, PeerCreate, 1)); // Existing endpoint copies cannot reconnect after shutdown.
    for (unsigned i = 0; i < TestClientCount; ++i) if (clients[i]) xpc_connection_cancel(clients[i]);
    stopPeer(&second); stopPeer(&third);
}

int main(int argc, char **argv) {
    alarm(60);
    @autoreleasepool {
        if (argc == 2 && !strcmp(argv[1], "--public-client")) return runPeer(100, 101);
        int error = 42;
        LSBIOSurfaceServer *server = [[LSBIOSurfaceServer alloc] initWithError:&error];
        assert(server && !error && MACH_PORT_VALID(server.registrationPort));
        assert(!server.connectedClientPID);
        xpc_connection_t client = clientFor(server);
        // Method 0 is the stock simulator IOSurface client's initial limits handshake.
        xpc_object_t reply = request(client, 0);
        assert(xpc_get_type(reply) == XPC_TYPE_DICTIONARY);
        size_t length = 0;
        assert(xpc_dictionary_get_data(reply, "Limits", &length) && length >= 40);
        assert(server.connectedClientPID == getpid());
        testSurfaceAllocation(client);
        [server close];
        assert(!server.registrationPort && !server.connectedClientPID);
        awaitDisconnected(client);
        xpc_connection_cancel(client);
        [server close]; // Shutdown is idempotent, including when followed by deallocation.
        server = nil;
        server = [[LSBIOSurfaceServer alloc] initWithError:&error];
        assert(server && !error);
        server = nil;
        server = [[LSBIOSurfaceServer alloc] initWithError:&error];
        assert(server && !error);
        testPublicClients(argv[0], server);
    }
    puts("IOSurfaceServer: public clients in three native processes, stock shared surfaces, live-peer limit and shutdown passed");
    return 0;
}
