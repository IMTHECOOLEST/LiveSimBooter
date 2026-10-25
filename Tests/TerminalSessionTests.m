#import <Foundation/Foundation.h>
#import "../Host/TerminalSession.h"
#include "../ProcessSupport/FakePTY.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

static void pumpUntil(BOOL (^finished)(void)) {
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:2];
    while (!finished() && deadline.timeIntervalSinceNow > 0) {
        [NSRunLoop.mainRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    }
    assert(finished());
}

int main(void) {
    @autoreleasepool {
        char directory[] = "/tmp/LiveSimBooter-terminal-session.XXXXXX";
        assert(mkdtemp(directory));
        int error = 0;
        LSBTerminalSession *session = [[LSBTerminalSession alloc] initWithDirectory:[NSString stringWithUTF8String:directory] error:&error];
        assert(session && !error);
        NSString *statePath = session.statePath;
        LSBReceivedMessage message = {0};
        for (unsigned index = 0; index < LSB_MAX_FDS; ++index) message.fds[index] = -1;
        assert(![session attachToMessage:&message] && message.plan.fdCount == 3);
        int stateFD = open(statePath.fileSystemRepresentation, O_RDWR);
        assert(stateFD >= 0);
        LSBFakePTYState *state = mmap(NULL, LSB_FAKE_PTY_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, stateFD, 0);
        assert(state != MAP_FAILED && !LSBFakePTYValidate(state, LSB_FAKE_PTY_MAP_SIZE));
        close(stateFD);
        assert(LSBFakePTYMatchesFD(state, message.fds[0]) && LSBFakePTYMatchesFD(state, message.fds[2]));
        __block NSMutableData *output = [NSMutableData new];
        __block int signalNumber;
        session.outputHandler = ^(NSData *data) { [output appendData:data]; };
        session.signalHandler = ^int(pid_t group, int number) { (void)group; signalNumber = number; return 0; };
        [session start];
        assert(write(message.fds[1], "session\n", 8) == 8);
        pumpUntil(^BOOL { return output.length >= 9; });
        assert([output isEqualToData:[@"session\r\n" dataUsingEncoding:NSUTF8StringEncoding]]);
        struct termios attributes;
        assert(!LSBFakePTYGetAttributes(state, &attributes));
        cfmakeraw(&attributes);
        assert(!LSBFakePTYSetAttributes(state, &attributes));
        NSData *input = [@"abc\033[D\r" dataUsingEncoding:NSUTF8StringEncoding];
        assert(![session sendInput:input]);
        char bytes[64];
        ssize_t count = read(message.fds[0], bytes, sizeof(bytes));
        assert(count == (ssize_t)input.length && !memcmp(bytes, input.bytes, input.length));
        [session resizeRows:37 columns:101];
        struct winsize size;
        assert(!LSBFakePTYGetWindow(state, &size) && size.ws_row == 37 && size.ws_col == 101 && signalNumber == SIGWINCH);
        attributes.c_lflag |= ISIG;
        assert(!LSBFakePTYSetAttributes(state, &attributes));
        assert(![session sendInput:[NSData dataWithBytes:"\003" length:1]] && signalNumber == SIGINT);

        // Keep the socket readable from each output callback. Main-queue work must run before this stream ends.
        int savedOutput = dup(STDOUT_FILENO), nullOutput = open("/dev/null", O_WRONLY);
        assert(savedOutput >= 0 && nullOutput >= 0 && dup2(nullOutput, STDOUT_FILENO) >= 0);
        close(nullOutput);
        __block unsigned callbacks = 0, callbacksBeforeYield = 0;
        __block BOOL yielded = NO;
        NSData *continuousOutput = [NSMutableData dataWithLength:4096];
        session.outputHandler = ^(NSData *data) {
            assert(data.length == continuousOutput.length);
            if (!callbacks++) dispatch_async(dispatch_get_main_queue(), ^{ callbacksBeforeYield = callbacks; yielded = YES; });
            if (callbacks < 128) assert(write(message.fds[1], continuousOutput.bytes, continuousOutput.length) == (ssize_t)continuousOutput.length);
        };
        assert(write(message.fds[1], continuousOutput.bytes, continuousOutput.length) == (ssize_t)continuousOutput.length);
        pumpUntil(^BOOL { return yielded; });
        assert(callbacksBeforeYield > 0 && callbacksBeforeYield <= 8);
        session.outputHandler = nil;
        for (unsigned index = 0; index < message.plan.fdCount; ++index) close(message.fds[index]);
        pumpUntil(^BOOL { return access(statePath.fileSystemRepresentation, F_OK) < 0; });
        fflush(stdout);
        assert(dup2(savedOutput, STDOUT_FILENO) >= 0);
        close(savedOutput);
        assert([session sendInput:input] == EIO);
        [session close];
        munmap(state, LSB_FAKE_PTY_MAP_SIZE);
        assert(!rmdir(directory));
        puts("TerminalSession: inherited endpoint, fair dispatch I/O, raw input, output translation, resize/signals and EOF cleanup passed");
    }
    return 0;
}
