#include "SpawnTransport.h"
#include "PrivateXPC.h"
#include "SingleThreadGuard.h"
#include "ChildStatus.h"
#if defined(LSB_XPC_WIRE_COMPATIBILITY)
#include "../Guest/XPCWireCompatibility.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(LSB_TRANSPORT_TRACE)
#include <stdio.h>
#endif

#define LSB_TRANSPORT_VERSION 1

static int traceError(const char *stage, int error, unsigned detail) {
#if defined(LSB_TRANSPORT_TRACE)
    if (error) {
        int savedErrno = errno;
        fprintf(stderr, "LiveSimBooter XPC: %s failed: error=%d detail=%u\n", stage, error, detail);
        errno = savedErrno;
    }
#else
    (void)stage;
    (void)detail;
#endif
    return error;
}

static bool unsignedValue(xpc_object_t dictionary, const char *key, uint64_t *value) {
    xpc_object_t object = xpc_dictionary_get_value(dictionary, key);
    if (!object || xpc_get_type(object) != XPC_TYPE_UINT64) return false;
    *value = xpc_uint64_get_value(object);
    return true;
}

static bool signedValue(xpc_object_t dictionary, const char *key, int64_t *value) {
    xpc_object_t object = xpc_dictionary_get_value(dictionary, key);
    if (!object || xpc_get_type(object) != XPC_TYPE_INT64) return false;
    *value = xpc_int64_get_value(object);
    return true;
}

static void initializeFDs(int fds[LSB_MAX_FDS]) {
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) fds[i] = -1;
}

static void closeFDs(int fds[LSB_MAX_FDS]) {
    for (unsigned i = 0; i < LSB_MAX_FDS; ++i) {
        if (fds[i] >= 0) close(fds[i]);
        fds[i] = -1;
    }
}

static bool sendRight(mach_port_t port) {
    mach_port_type_t type = 0;
    return MACH_PORT_VALID(port) && !mach_port_type(mach_task_self(), port, &type) && (type & MACH_PORT_TYPE_SEND);
}

static int encodePorts(xpc_object_t dictionary, const LSBSpawnPorts *ports) {
    if (!ports) return 0;
    if (ports->count > LSB_MAX_REGISTERED_PORTS || (!ports->registeredPresent && ports->count)) return EINVAL;
    for (unsigned i = 0; i < LSB_MAX_REGISTERED_PORTS; ++i) {
        mach_port_t port = ports->registered[i];
        if (i >= ports->count) { if (port != MACH_PORT_NULL) return EINVAL; else continue; }
        if (port == MACH_PORT_NULL) continue;
        if (!sendRight(port)) return EBADF;
    }
    if (!ports->bootstrapPresent && ports->bootstrapPort != MACH_PORT_NULL) return EINVAL;
    if (ports->bootstrapPort != MACH_PORT_NULL && !sendRight(ports->bootstrapPort)) return EBADF;
    if (ports->registeredPresent) {
        xpc_object_t array = xpc_array_create(NULL, 0);
        if (!array) return ENOMEM;
        for (unsigned i = 0; i < ports->count; ++i) xpc_array_set_mach_send(array, XPC_ARRAY_APPEND, ports->registered[i]);
        xpc_dictionary_set_value(dictionary, "registered-ports", array);
        xpc_release(array);
    }
    if (ports->bootstrapPresent) xpc_dictionary_set_mach_send(dictionary, "bootstrap-port", ports->bootstrapPort);
    return 0;
}

static int decodePorts(xpc_object_t dictionary, LSBSpawnPorts *ports) {
    xpc_object_t array = xpc_dictionary_get_value(dictionary, "registered-ports");
    if (array && (xpc_get_type(array) != XPC_TYPE_ARRAY || xpc_array_get_count(array) > LSB_MAX_REGISTERED_PORTS)) return EPROTO;
    ports->registeredPresent = array != NULL;
    ports->count = array ? (uint32_t)xpc_array_get_count(array) : 0;
    for (unsigned i = 0; i < ports->count; ++i) {
        xpc_object_t object = xpc_array_get_value(array, i);
        if (!object || xpc_get_type(object) != &_xpc_type_mach_send) { LSBSpawnPortsDispose(ports); return EPROTO; }
        mach_port_t borrowed = xpc_mach_send_get_right(object);
        if (borrowed == MACH_PORT_NULL) continue;
        mach_port_t copy = xpc_array_copy_mach_send(array, i);
        ports->registered[i] = copy;
        if (!sendRight(copy)) {
            LSBSpawnPortsDispose(ports);
            return EPROTO;
        }
    }
    xpc_object_t bootstrap = xpc_dictionary_get_value(dictionary, "bootstrap-port");
    if (bootstrap) {
        if (xpc_get_type(bootstrap) != &_xpc_type_mach_send) { LSBSpawnPortsDispose(ports); return EPROTO; }
        ports->bootstrapPresent = true;
        if (xpc_mach_send_get_right(bootstrap) != MACH_PORT_NULL) {
            ports->bootstrapPort = xpc_dictionary_copy_mach_send(dictionary, "bootstrap-port");
            if (!sendRight(ports->bootstrapPort)) { LSBSpawnPortsDispose(ports); return EPROTO; }
        }
    }
    return 0;
}

static int validateBootstrapFDs(int statusFD, int wakeFD) {
    struct stat status, wake;
    if (fstat(statusFD, &status) || fstat(wakeFD, &wake)) return errno;
    int statusFlags = fcntl(statusFD, F_GETFL), wakeFlags = fcntl(wakeFD, F_GETFL);
    if (statusFlags < 0 || wakeFlags < 0) return errno;
    if (!S_ISREG(status.st_mode) || status.st_size != LSB_CHILD_STATUS_MAP_SIZE || (statusFlags & O_ACCMODE) != O_RDWR ||
        !S_ISFIFO(wake.st_mode) || (wakeFlags & O_ACCMODE) != O_RDONLY || !(wakeFlags & O_NONBLOCK)) return EPROTO;
    return 0;
}

static int decodeBootstrapFD(xpc_object_t dictionary, const char *key, int *fd) {
    xpc_object_t object = xpc_dictionary_get_value(dictionary, key);
    if (!object || xpc_get_type(object) != XPC_TYPE_FD) return EPROTO;
    *fd = xpc_fd_dup(object);
    if (*fd < 0) return errno ? errno : EBADF;
    if (fcntl(*fd, F_SETFD, FD_CLOEXEC)) return errno;
    return 0;
}

static int encodePlan(xpc_object_t dictionary, const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]) {
    int error = LSBValidateSpawnPlan(plan);
    if (error) return traceError("validate plan", error, 0);
    if (plan->fdCount && !fds) return traceError("missing FDs", EINVAL, plan->fdCount);
    xpc_object_t array = xpc_array_create(NULL, 0);
    if (!array) return traceError("create FD array", ENOMEM, plan->fdCount);
    for (unsigned i = 0; i < plan->fdCount; ++i) {
        if (fds[i] < 0) { error = traceError("negative FD", EBADF, i); break; }
        xpc_object_t descriptor = xpc_fd_create(fds[i]);
        if (!descriptor) { error = traceError("xpc_fd_create", errno ? errno : EBADF, (unsigned)fds[i]); break; }
        xpc_array_append_value(array, descriptor);
        xpc_release(descriptor);
    }
    if (!error) {
        xpc_dictionary_set_data(dictionary, "plan", plan, sizeof(*plan));
        xpc_dictionary_set_value(dictionary, "fds", array);
    }
    xpc_release(array);
    return error;
}

static int decodePlan(xpc_object_t dictionary, LSBSpawnPlan *plan, int fds[LSB_MAX_FDS]) {
    xpc_object_t data = xpc_dictionary_get_value(dictionary, "plan");
    xpc_object_t array = xpc_dictionary_get_value(dictionary, "fds");
    if (!data || xpc_get_type(data) != XPC_TYPE_DATA || xpc_data_get_length(data) != sizeof(*plan) ||
        !array || xpc_get_type(array) != XPC_TYPE_ARRAY) return EPROTO;
    memcpy(plan, xpc_data_get_bytes_ptr(data), sizeof(*plan));
    int error = LSBValidateSpawnPlan(plan);
    if (error) return error;
    if (xpc_array_get_count(array) != plan->fdCount) return EPROTO;
    for (unsigned i = 0; i < plan->fdCount; ++i) {
        xpc_object_t descriptor = xpc_array_get_value(array, i);
        if (!descriptor || xpc_get_type(descriptor) != XPC_TYPE_FD) { error = EPROTO; break; }
        fds[i] = xpc_fd_dup(descriptor);
        if (fds[i] < 0) { error = errno ? errno : EBADF; break; }
    }
    if (error) closeFDs(fds);
    return error;
}

int LSBTransportCopyBrokerPort(mach_port_t *port) {
    if (!port) return EINVAL;
    *port = MACH_PORT_NULL;
#if defined(LSB_GUEST_RUNTIME)
    // Mach names remain valid across the in-process dyld handoff. Simulator libxpc deliberately disables its
    // bootstrap lookup without a simulator launchd, so use the right obtained by our native entry instead.
    const char *text = getenv(LSB_BROKER_PORT_ENV);
    if (!text) return ENOENT;
    uint64_t value = 0;
    if (!*text) return EINVAL;
    for (unsigned i = 0; text[i]; ++i) {
        if (i >= 10 || text[i] < '0' || text[i] > '9') return EINVAL;
        value = value * 10 + (unsigned)(text[i] - '0');
        if (value >= UINT32_MAX) return EINVAL;
    }
    if (!value) return EINVAL;
    mach_port_t candidate = (mach_port_t)value;
    mach_port_type_t type = 0;
    if (mach_port_type(mach_task_self(), candidate, &type) || !(type & MACH_PORT_TYPE_SEND)) return EACCES;
    if (mach_port_mod_refs(mach_task_self(), candidate, MACH_PORT_RIGHT_SEND, 1)) return EACCES;
    *port = candidate;
    return 0;
#elif defined(LSB_TRANSPORT_TESTING)
    extern int LSBTestCopyBrokerPort(mach_port_t *port);
    return LSBTestCopyBrokerPort(port);
#else
    kern_return_t result = bootstrap_look_up(bootstrap_port, LSB_BROKER_SERVICE, port);
    if (result == KERN_SUCCESS) return 0;
    if (result == 1100) return EACCES; // BOOTSTRAP_NOT_PRIVILEGED
    if (result == 1102) return ENOENT; // BOOTSTRAP_UNKNOWN_SERVICE
    return EIO;
#endif
}

static xpc_object_t createRequest(uint32_t kind, uint64_t requestID, int error) {
    xpc_object_t request = xpc_dictionary_create(NULL, NULL, 0);
    if (!request) return NULL;
    xpc_dictionary_set_uint64(request, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(request, "kind", kind);
    xpc_dictionary_set_uint64(request, "request-id", requestID);
    xpc_dictionary_set_int64(request, "error", error);
    return request;
}

// This private API has no documented timeout. The native broker must finish or fail each accepted request.
// Nothing from this call (including a pipe or reply object) may survive the native-cache handoff.
static int callBrokerOnPort(mach_port_t port, xpc_object_t request, xpc_object_t *reply) {
    *reply = NULL;
    if (!sendRight(port)) return EINVAL;
    xpc_object_t pipe = xpc_pipe_create_from_port(port, 0);
    if (!pipe) return traceError("xpc_pipe_create_from_port", ENOMEM, port);
    int error;
#if defined(LSB_XPC_WIRE_COMPATIBILITY)
    error = LSBXPCWireBegin(port);
    if (!error) {
        error = xpc_pipe_routine(pipe, request, reply);
        LSBXPCWireEnd();
    }
#else
    error = xpc_pipe_routine(pipe, request, reply);
#endif
    if (error) traceError("xpc_pipe_routine", error, port);
    xpc_release(pipe);
    if (error && *reply) { xpc_release(*reply); *reply = NULL; }
    return error;
}

static int callBroker(xpc_object_t request, xpc_object_t *reply) {
    *reply = NULL;
    mach_port_t port = MACH_PORT_NULL;
    int error = LSBTransportCopyBrokerPort(&port);
    if (error) return traceError("copy broker port", error, port);
    error = callBrokerOnPort(port, request, reply);
    mach_port_deallocate(mach_task_self(), port);
    return error;
}

static int decodeReplyHeader(xpc_object_t reply, int *remoteError, pid_t *pid, uint64_t *requestID) {
    uint64_t version;
    int64_t encodedError, encodedPID;
    if (!reply || xpc_get_type(reply) != XPC_TYPE_DICTIONARY || !unsignedValue(reply, "version", &version) ||
        version != LSB_TRANSPORT_VERSION || !unsignedValue(reply, "request-id", requestID) ||
        !signedValue(reply, "error", &encodedError) || encodedError < 0 || encodedError > INT_MAX ||
        !signedValue(reply, "pid", &encodedPID) || encodedPID < 0 || encodedPID > INT_MAX) return EPROTO;
    *remoteError = (int)encodedError;
    *pid = (pid_t)encodedPID;
    return 0;
}

static int processRequest(uint32_t kind, const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports, pid_t *pid) {
    if (!pid) return EINVAL;
    *pid = 0;
    xpc_object_t request = createRequest(kind, 0, 0);
    if (!request) return traceError("create process request", ENOMEM, kind);
    int error = encodePlan(request, plan, fds);
    if (!error) error = encodePorts(request, ports);
    xpc_object_t reply = NULL;
    if (!error) error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, pid, &requestID);
        if (error) traceError("decode process reply", error, kind);
        if (!error && remoteError) traceError("broker process reply", remoteError, kind);
        if (!error) error = remoteError;
        if (!error && *pid <= 0) error = EPROTO;
    }
    if (reply) xpc_release(reply);
    if (error) *pid = 0;
    return error;
}

int LSBTransportSpawnWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports, pid_t *pid) {
    return processRequest(LSB_REQUEST_SPAWN, plan, fds, ports, pid);
}

int LSBTransportExecWithPorts(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], const LSBSpawnPorts *ports) {
    pid_t pid = 0;
    return processRequest(LSB_REQUEST_EXEC, plan, fds, ports, &pid);
}

int LSBTransportSpawn(const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], pid_t *pid) {
    return LSBTransportSpawnWithPorts(plan, fds, NULL, pid);
}

int LSBTransportCheckIn(LSBBootstrapReply *result, int fds[LSB_MAX_FDS]) {
    if (!result || !fds) return EINVAL;
    memset(result, 0, sizeof(*result));
    result->statusFD = result->wakeFD = -1;
    initializeFDs(fds);
    xpc_object_t request = createRequest(LSB_REQUEST_CHECKIN, 0, 0);
    if (!request) return ENOMEM;
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &result->requestID);
        result->error = remoteError;
        result->pid = pid;
        if (!error) error = remoteError;
        if (!error && (!result->requestID || pid <= 0)) error = EPROTO;
        if (!error) {
            int64_t guestPID, parentPID;
            if (!signedValue(reply, "guest-pid", &guestPID) || guestPID <= 0 || guestPID > INT_MAX ||
                !signedValue(reply, "parent-pid", &parentPID) || parentPID < 0 || parentPID > INT_MAX) error = EPROTO;
            else { result->guestPID = (int32_t)guestPID; result->parentPID = (int32_t)parentPID; }
        }
        if (!error) {
            xpc_object_t root = xpc_dictionary_get_value(reply, "runtime-root");
            if (!root || xpc_get_type(root) != XPC_TYPE_STRING || !xpc_string_get_length(root) ||
                xpc_string_get_length(root) >= sizeof(result->runtimeRoot)) error = EPROTO;
            else memcpy(result->runtimeRoot, xpc_string_get_string_ptr(root), xpc_string_get_length(root) + 1);
        }
        if (!error) error = decodePlan(reply, &result->plan, fds);
        if (!error) error = decodeBootstrapFD(reply, "child-status-fd", &result->statusFD);
        if (!error) error = decodeBootstrapFD(reply, "child-wake-fd", &result->wakeFD);
        if (!error) error = validateBootstrapFDs(result->statusFD, result->wakeFD);
        if (!error) error = decodePorts(reply, &result->ports);
        if (!error) {
            xpc_object_t port = xpc_dictionary_get_value(reply, "launchd-tank-port");
            if (port) {
                if (xpc_get_type(port) != &_xpc_type_mach_send) error = EPROTO;
                else {
                    result->launchdTankPort = xpc_dictionary_copy_mach_send(reply, "launchd-tank-port");
                    if (!MACH_PORT_VALID(result->launchdTankPort)) error = EPROTO;
                }
            }
        }
    }
    if (reply) xpc_release(reply);
    if (error) {
        closeFDs(fds);
        if (result->statusFD >= 0) close(result->statusFD);
        if (result->wakeFD >= 0) close(result->wakeFD);
        result->statusFD = result->wakeFD = -1;
        if (MACH_PORT_VALID(result->launchdTankPort)) mach_port_deallocate(mach_task_self(), result->launchdTankPort);
        result->launchdTankPort = MACH_PORT_NULL;
        LSBSpawnPortsDispose(&result->ports);
    }
    return error;
}

int LSBTransportReadyOnPort(mach_port_t broker, uint64_t requestID, int childError) {
    if (!requestID || childError < 0) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_READY, requestID, childError);
    if (!request) return ENOMEM;
    xpc_object_t reply = NULL;
    int error = callBrokerOnPort(broker, request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t replyID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &replyID);
        if (!error && replyID != requestID) error = EPROTO;
        if (!error) error = remoteError;
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportReady(uint64_t requestID, int childError) {
    if (!requestID || childError < 0) return EINVAL;
    mach_port_t port = MACH_PORT_NULL;
    int error = LSBTransportCopyBrokerPort(&port);
    if (error) return error;
    error = LSBTransportReadyOnPort(port, requestID, childError);
    mach_port_deallocate(mach_task_self(), port);
    return error;
}

int LSBTransportKill(pid_t targetPID, int signalNumber) {
    if (signalNumber < 0 || signalNumber >= NSIG) return EINVAL;
    if (targetPID <= 0) return ENOTSUP;
    xpc_object_t request = createRequest(LSB_REQUEST_KILL, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_int64(request, "target-pid", targetPID);
    xpc_dictionary_set_int64(request, "signal", signalNumber);
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (requestID || (!remoteError && pid != targetPID))) error = EPROTO;
        if (!error) error = remoteError;
    }
    if (reply) xpc_release(reply);
    return error;
}

int LSBTransportResolveAuditPID(const audit_token_t *audit, pid_t *result) {
    if (result) *result = 0;
    if (!audit || !result || !audit->val[5] || audit->val[5] > INT_MAX) return EINVAL;
    xpc_object_t request = createRequest(LSB_REQUEST_RESOLVE_AUDIT_PID, 0, 0);
    if (!request) return ENOMEM;
    xpc_dictionary_set_data(request, "target-audit", audit, sizeof(*audit));
    xpc_object_t reply = NULL;
    int error = callBroker(request, &reply);
    xpc_release(request);
    if (!error) {
        int remoteError = 0;
        pid_t pid = 0;
        uint64_t requestID = 0;
        error = decodeReplyHeader(reply, &remoteError, &pid, &requestID);
        if (!error && (requestID || (!remoteError && pid <= 0))) error = EPROTO;
        if (!error) error = remoteError;
        if (!error) *result = pid;
    }
    if (reply) xpc_release(reply);
    return error;
}

void LSBTransportDisposeMessage(LSBReceivedMessage *message) {
    if (!message) return;
    if (message->object) xpc_release((xpc_object_t)message->object);
    message->object = NULL;
    closeFDs(message->fds);
    LSBSpawnPortsDispose(&message->ports);
}

static int replyToMessage(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                          const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], bool bootstrap, int statusFD, int wakeFD,
                          mach_port_t tankPort, const LSBSpawnPorts *ports, pid_t guestPID, pid_t parentPID) {
    if (!message || !message->object || error < 0 || pid < 0) return EINVAL;
    xpc_object_t reply = xpc_dictionary_create_reply((xpc_object_t)message->object);
    if (!reply) {
        xpc_release((xpc_object_t)message->object);
        message->object = NULL;
        return EPIPE;
    }
    int encodingError = 0;
    if (!error && plan) {
        if (!runtimeRoot || !runtimeRoot[0] || strnlen(runtimeRoot, LSB_PATH_BYTES) >= LSB_PATH_BYTES) encodingError = EINVAL;
        else {
            encodingError = encodePlan(reply, plan, fds);
            if (!encodingError) xpc_dictionary_set_string(reply, "runtime-root", runtimeRoot);
        }
    }
    if (!error && !encodingError && bootstrap) {
        encodingError = plan && pid > 0 && guestPID > 0 && parentPID >= 0 ? validateBootstrapFDs(statusFD, wakeFD) : EINVAL;
        if (!encodingError) {
            xpc_object_t status = xpc_fd_create(statusFD), wake = xpc_fd_create(wakeFD);
            if (!status || !wake) encodingError = errno ? errno : EBADF;
            else {
                xpc_dictionary_set_value(reply, "child-status-fd", status);
                xpc_dictionary_set_value(reply, "child-wake-fd", wake);
            }
            if (status) xpc_release(status);
            if (wake) xpc_release(wake);
        }
        if (!encodingError && MACH_PORT_VALID(tankPort)) xpc_dictionary_set_mach_send(reply, "launchd-tank-port", tankPort);
        if (!encodingError) encodingError = encodePorts(reply, ports);
        if (!encodingError) {
            xpc_dictionary_set_int64(reply, "guest-pid", guestPID);
            xpc_dictionary_set_int64(reply, "parent-pid", parentPID);
        }
    }
    if (encodingError) error = encodingError;
    xpc_dictionary_set_uint64(reply, "version", LSB_TRANSPORT_VERSION);
    xpc_dictionary_set_uint64(reply, "request-id", requestID);
    xpc_dictionary_set_int64(reply, "error", error);
    xpc_dictionary_set_int64(reply, "pid", error ? 0 : pid);
    int sendError = xpc_pipe_routine_reply(reply);
    xpc_release(reply);
    xpc_release((xpc_object_t)message->object);
    message->object = NULL;
    return sendError ? sendError : encodingError;
}

int LSBTransportReply(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS]) {
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, false, -1, -1, MACH_PORT_NULL, NULL, 0, 0);
}

int LSBTransportReplyBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                               const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD) {
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, true, statusFD, wakeFD, MACH_PORT_NULL, NULL, pid, getpid());
}

int LSBTransportReplyBootstrapWithPorts(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                        const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, const LSBSpawnPorts *ports) {
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, true, statusFD, wakeFD, MACH_PORT_NULL, ports, pid, getpid());
}

int LSBTransportReplyBootstrapWithMetadata(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                           const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD,
                                           const LSBBootstrapMetadata *metadata) {
    if (!metadata || (metadata->launchdTankPort != MACH_PORT_NULL && !sendRight(metadata->launchdTankPort))) return EINVAL;
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, true, statusFD, wakeFD,
                          metadata->launchdTankPort, metadata->ports, metadata->guestPID, metadata->parentPID);
}

int LSBTransportReplyLaunchdBootstrap(LSBReceivedMessage *message, int error, pid_t pid, uint64_t requestID, const char *runtimeRoot,
                                      const LSBSpawnPlan *plan, const int fds[LSB_MAX_FDS], int statusFD, int wakeFD, mach_port_t tankPort) {
    if (!MACH_PORT_VALID(tankPort)) return EINVAL;
    return replyToMessage(message, error, pid, requestID, runtimeRoot, plan, fds, true, statusFD, wakeFD, tankPort, NULL, pid, getpid());
}

int LSBTransportReceive(mach_port_t port, LSBReceivedMessage *message) {
    if (!message || !MACH_PORT_VALID(port)) return EINVAL;
    memset(message, 0, sizeof(*message));
    initializeFDs(message->fds);
    xpc_object_t request = NULL;
    int error = xpc_pipe_receive(port, &request, 0);
    if (error || !request) {
        if (request) xpc_release(request);
        return error ? error : EPROTO;
    }
    if (xpc_get_type(request) != XPC_TYPE_DICTIONARY) { xpc_release(request); return EPROTO; }
    message->object = request;
    xpc_dictionary_get_audit_token(request, &message->audit);
    uint64_t version = 0, kind = 0;
    int64_t childError = 0;
    if (!unsignedValue(request, "version", &version) || version != LSB_TRANSPORT_VERSION ||
        !unsignedValue(request, "kind", &kind) || kind < LSB_REQUEST_SPAWN || kind > LSB_REQUEST_RESOLVE_AUDIT_PID ||
        !unsignedValue(request, "request-id", &message->requestID) || !signedValue(request, "error", &childError) ||
        childError < 0 || childError > INT_MAX) error = EPROTO;
    if (!error) {
        message->kind = (uint32_t)kind;
        message->error = (int)childError;
        if (kind == LSB_REQUEST_READY) {
            if (!message->requestID || xpc_dictionary_get_value(request, "plan") || xpc_dictionary_get_value(request, "fds") ||
                xpc_dictionary_get_value(request, "registered-ports") || xpc_dictionary_get_value(request, "bootstrap-port")) error = EPROTO;
        } else if (message->requestID || childError) error = EPROTO;
        else if (kind == LSB_REQUEST_SPAWN || kind == LSB_REQUEST_EXEC) {
            error = decodePlan(request, &message->plan, message->fds);
            if (!error) error = decodePorts(request, &message->ports);
        } else if (xpc_dictionary_get_value(request, "plan") || xpc_dictionary_get_value(request, "fds") ||
                   xpc_dictionary_get_value(request, "registered-ports") || xpc_dictionary_get_value(request, "bootstrap-port")) error = EPROTO;
        if (!error && kind == LSB_REQUEST_KILL) {
            int64_t target = 0, signal = 0;
            if (!signedValue(request, "target-pid", &target) || target < INT_MIN || target > INT_MAX ||
                !signedValue(request, "signal", &signal) || signal < INT_MIN || signal > INT_MAX) error = EPROTO;
            else { message->targetPID = (pid_t)target; message->signalNumber = (int)signal; }
        }
        if (!error && kind == LSB_REQUEST_RESOLVE_AUDIT_PID) {
            xpc_object_t target = xpc_dictionary_get_value(request, "target-audit");
            if (!target || xpc_get_type(target) != XPC_TYPE_DATA || xpc_data_get_length(target) != sizeof(message->targetAudit) ||
                xpc_dictionary_get_value(request, "target-pid") || xpc_dictionary_get_value(request, "signal")) error = EPROTO;
            else {
                memcpy(&message->targetAudit, xpc_data_get_bytes_ptr(target), sizeof(message->targetAudit));
                if (!message->targetAudit.val[5] || message->targetAudit.val[5] > INT_MAX) error = EPROTO;
            }
        } else if (!error && xpc_dictionary_get_value(request, "target-audit")) error = EPROTO;
    }
    if (error) {
        (void)LSBTransportReply(message, error, 0, message->requestID, NULL, NULL, NULL);
        LSBTransportDisposeMessage(message);
    }
    return error;
}

int LSBRequireSingleThread(void) {
    return LSBSingleThreadGuard();
}
