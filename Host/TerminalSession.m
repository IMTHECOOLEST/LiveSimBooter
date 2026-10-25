#import "TerminalSession.h"
#include "FakePTY.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

@interface LSBTerminalSession () {
    int _master, _slave, _stateFD, _consoleFD;
    LSBFakePTYState *_state;
    LSBFakePTYDiscipline _discipline;
    dispatch_source_t _reader, _writer, _consoleReader;
    dispatch_group_t _masterRegistrations;
    NSMutableData *_pendingInput;
    NSString *_inputPath;
    BOOL _started, _closed;
    uint32_t _inputFlush;
}
@property (nonatomic, readwrite) NSString *statePath;
- (int)queueInput:(const void *)bytes length:(size_t)length;
- (void)drainInput;
- (void)emitOutput:(const void *)bytes length:(size_t)length;
@end

static int terminalInput(void *context, const void *bytes, size_t length) {
    return [(__bridge LSBTerminalSession *)context queueInput:bytes length:length];
}
static int terminalOutput(void *context, const void *bytes, size_t length) {
    [(__bridge LSBTerminalSession *)context emitOutput:bytes length:length];
    return 0;
}
static int terminalSignal(void *context, pid_t group, int number) {
    LSBTerminalSession *session = (__bridge LSBTerminalSession *)context;
    return session.signalHandler ? session.signalHandler(group, number) : ESRCH;
}

@implementation LSBTerminalSession
- (instancetype)initWithDirectory:(NSString *)directory error:(int *)error {
    if (!(self = [super init])) { *error = ENOMEM; return nil; }
    _master = _slave = _stateFD = _consoleFD = -1;
    _pendingInput = [NSMutableData new];
    _masterRegistrations = dispatch_group_create();
    char path[PATH_MAX];
    int length = snprintf(path, sizeof(path), "%s/.terminal.XXXXXX", directory.fileSystemRepresentation);
    *error = length < 0 || (size_t)length >= sizeof(path) ? ENAMETOOLONG : 0;
    if (!*error && (_stateFD = mkstemp(path)) < 0) *error = errno;
    if (!*error) self.statePath = [NSString stringWithUTF8String:path];
    if (!*error && (fcntl(_stateFD, F_SETFD, FD_CLOEXEC) || ftruncate(_stateFD, LSB_FAKE_PTY_MAP_SIZE))) *error = errno;
    if (!*error) {
        void *mapping = mmap(NULL, LSB_FAKE_PTY_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, _stateFD, 0);
        if (mapping == MAP_FAILED) *error = errno;
        else _state = mapping;
    }
    int sockets[2];
    if (!*error && socketpair(AF_UNIX, SOCK_STREAM, 0, sockets)) *error = errno;
    else if (!*error) {
        _master = sockets[0]; _slave = sockets[1];
        int enabled = 1;
        for (unsigned i = 0; i < 2 && !*error; ++i) {
            if (fcntl(sockets[i], F_SETFD, FD_CLOEXEC) || setsockopt(sockets[i], SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)))
                *error = errno;
        }
        if (!*error && fcntl(_master, F_SETFL, O_NONBLOCK)) *error = errno;
    }
    if (!*error) *error = LSBFakePTYInitialize(_state, _slave, 24, 80);
    if (*error) { [self close]; return nil; }
    return self;
}

- (int)attachToMessage:(LSBReceivedMessage *)message {
    if (_slave < 0 || message->plan.fdCount) return EINVAL;
    char environment[PATH_MAX + 32];
    int length = snprintf(environment, sizeof(environment), LSB_FAKE_PTY_ENV "=%s", self.statePath.fileSystemRepresentation);
    if (length < 0 || (size_t)length >= sizeof(environment)) return ENAMETOOLONG;
    LSBSpawnPlan *plan = &message->plan;
    if (plan->envc == LSB_MAX_VECTOR) return E2BIG;
    int error = LSBPlanAddString(plan, environment, &plan->envp[plan->envc]);
    if (error) return error;
    ++plan->envc;
    for (int descriptor = 0; descriptor < 3; ++descriptor) {
        int duplicate = fcntl(_slave, F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0) return errno;
        message->fds[plan->fdCount] = duplicate;
        plan->fds[plan->fdCount++] = (LSBInheritedFD){descriptor, 0};
    }
    close(_slave); _slave = -1;
    return 0;
}

- (void)start {
    if (_started || _closed) return;
    _started = YES;
    __weak LSBTerminalSession *weakSelf = self;
    _reader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, _master, 0, dispatch_get_main_queue());
    dispatch_group_t registrations = _masterRegistrations;
    dispatch_group_enter(registrations);
    dispatch_source_set_cancel_handler(_reader, ^{ dispatch_group_leave(registrations); });
    dispatch_source_set_event_handler(_reader, ^{
        LSBTerminalSession *session = weakSelf;
        if (!session || session->_closed) return;
        unsigned char buffer[8192];
        // A continuously writing guest must yield to keyboard, signal and broker events on this same queue.
        for (unsigned reads = 0; reads < 8; ++reads) {
            ssize_t count = read(session->_master, buffer, sizeof(buffer));
            if (count > 0) {
                LSBFakePTYCallbacks callbacks = {(__bridge void *)session, terminalInput, terminalOutput, terminalSignal};
                int error = LSBFakePTYOutput(session->_state, buffer, (size_t)count, &callbacks);
                if (error) fprintf(stderr, "[terminal] output: %s\n", strerror(error));
            } else if (count < 0 && errno == EINTR) continue;
            else {
                if (!count || (errno != EAGAIN && errno != EWOULDBLOCK)) [session close];
                break;
            }
        }
    });
    dispatch_resume(_reader);
    // devicectl --console can supply a pipe or terminal on stdin. A regular file or /dev/null is not an input event source.
    struct stat metadata;
    const char *debugInput = getenv("LSB_TERMINAL_INPUT_FIFO");
    if (debugInput && !strcmp(debugInput, "1")) {
        // Opt-in device regression input. devicectl's console does not consistently forward stdin on iOS.
        _inputPath = [self.statePath stringByAppendingString:@".input"];
        if (!mkfifo(_inputPath.fileSystemRepresentation, 0600)) {
            _consoleFD = open(_inputPath.fileSystemRepresentation, O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (_consoleFD >= 0) fprintf(stderr, "[terminal] input FIFO=%s\n", _inputPath.fileSystemRepresentation);
            else fprintf(stderr, "[terminal] open input FIFO %s: %s\n", _inputPath.fileSystemRepresentation, strerror(errno));
        } else fprintf(stderr, "[terminal] create input FIFO %s: %s\n", _inputPath.fileSystemRepresentation, strerror(errno));
    } else if (!fstat(STDIN_FILENO, &metadata) && (S_ISFIFO(metadata.st_mode) || isatty(STDIN_FILENO))) {
        _consoleFD = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 3);
    }
    if (_consoleFD >= 0) {
            _consoleReader = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, _consoleFD, 0, dispatch_get_main_queue());
            int console = _consoleFD;
            dispatch_source_set_cancel_handler(_consoleReader, ^{ close(console); });
            dispatch_source_set_event_handler(_consoleReader, ^{
                LSBTerminalSession *session = weakSelf;
                if (!session || session->_closed) return;
                char bytes[4096];
                ssize_t count = read(session->_consoleFD, bytes, sizeof(bytes));
                if (count > 0) {
                    int error = [session sendInput:[NSData dataWithBytes:bytes length:(NSUInteger)count]];
                    if (error) fprintf(stderr, "[terminal] console input: %s\n", strerror(error));
                }
                else if (!count || errno != EINTR) {
                    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
                    dispatch_source_cancel(session->_consoleReader);
                    session->_consoleReader = nil;
                    session->_consoleFD = -1;
                    // EOF on the console does not hang up the on-screen terminal.
                }
            });
            dispatch_resume(_consoleReader);
    }
}

- (void)drainInput {
    if (_closed) return;
    uint32_t generation = atomic_load_explicit(&_state->inputFlush, memory_order_acquire);
    if (generation != _inputFlush) {
        _inputFlush = generation;
        [_pendingInput setLength:0];
    }
    size_t budget = 65536;
    unsigned attempts = 8;
    while (_pendingInput.length && !_closed && budget && attempts--) {
        ssize_t count = write(_master, _pendingInput.bytes, MIN(_pendingInput.length, budget));
        if (count > 0) {
            budget -= (size_t)count;
            [_pendingInput replaceBytesInRange:NSMakeRange(0, (NSUInteger)count) withBytes:NULL length:0];
        }
        else if (count < 0 && errno == EINTR) continue;
        else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        else { [self close]; return; }
    }
    if (!_pendingInput.length && _writer) { dispatch_source_cancel(_writer); _writer = nil; }
}
- (int)queueInput:(const void *)bytes length:(size_t)length {
    if (_closed) return EIO;
    [self drainInput];
    if (_closed) return EIO;
    if (length > 1048576 - _pendingInput.length) return ENOBUFS;
    [_pendingInput appendBytes:bytes length:length];
    [self drainInput];
    if (_closed) return EIO;
    if (_pendingInput.length && !_writer) {
        __weak LSBTerminalSession *weakSelf = self;
        _writer = dispatch_source_create(DISPATCH_SOURCE_TYPE_WRITE, _master, 0, dispatch_get_main_queue());
        dispatch_group_t registrations = _masterRegistrations;
        dispatch_group_enter(registrations);
        dispatch_source_set_cancel_handler(_writer, ^{ dispatch_group_leave(registrations); });
        dispatch_source_set_event_handler(_writer, ^{ [weakSelf drainInput]; });
        dispatch_resume(_writer);
    }
    return 0;
}
- (void)emitOutput:(const void *)bytes length:(size_t)length {
    if (self.outputHandler) self.outputHandler([NSData dataWithBytes:bytes length:length]);
    // Preserve the raw terminal stream in a connected devicectl console for reproducible diagnostics.
    if (length) fwrite(bytes, 1, length, stdout);
    fflush(stdout);
}
- (int)sendInput:(NSData *)data {
    if (_closed) return EIO;
    LSBFakePTYCallbacks callbacks = {(__bridge void *)self, terminalInput, terminalOutput, terminalSignal};
    return LSBFakePTYInput(_state, &_discipline, data.bytes, data.length, &callbacks);
}
- (void)resizeRows:(unsigned short)rows columns:(unsigned short)columns {
    if (_closed || !rows || !columns) return;
    struct winsize previous, size = {.ws_row = rows, .ws_col = columns};
    if (LSBFakePTYGetWindow(_state, &previous) || !memcmp(&previous, &size, sizeof(size))) return;
    if (!LSBFakePTYSetWindow(_state, &size) && self.signalHandler) {
        pid_t group = 0;
        if (!LSBFakePTYGetForeground(_state, &group)) self.signalHandler(group, SIGWINCH);
    }
}
- (void)close {
    if (_closed) return;
    _closed = YES;
    // The kevent registrations must be cancelled before closing a watched descriptor, even on the same serial queue.
    if (_reader) dispatch_source_cancel(_reader);
    if (_writer) dispatch_source_cancel(_writer);
    if (_consoleReader) dispatch_source_cancel(_consoleReader);
    _reader = _writer = _consoleReader = nil;
    int master = _master;
    dispatch_group_notify(_masterRegistrations, dispatch_get_main_queue(), ^{
        if (master >= 0) close(master);
    });
    if (_slave >= 0) close(_slave);
    if (_stateFD >= 0) close(_stateFD);
    _master = _slave = _stateFD = _consoleFD = -1;
    if (_state) munmap(_state, LSB_FAKE_PTY_MAP_SIZE);
    _state = NULL;
    if (self.statePath) unlink(self.statePath.fileSystemRepresentation);
    if (_inputPath) unlink(_inputPath.fileSystemRepresentation);
}
- (void)dealloc { [self close]; }
@end
