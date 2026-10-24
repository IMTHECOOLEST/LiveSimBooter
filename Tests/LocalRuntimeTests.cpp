#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <mach/mach.h>
#include <unistd.h>

#if !defined(LSB_LOCAL_RUNTIME_RAW_TESTING)
#define LSB_LOCAL_RUNTIME_TESTING 1
#endif
#include "../LiveSimBooter/LocalRuntime.cpp"

namespace {

#if defined(LSB_LOCAL_RUNTIME_TESTING)
std::string sLog;
std::mutex sLogMutex;
unsigned sInterruptedWrites;
size_t sWriteLimit = SIZE_MAX;
bool sWriteFails;
uintptr_t sLastArguments[9];
int64_t sLastTrap;
#endif

void logMessage(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    lsb::vlog(format, arguments);
    va_end(arguments);
}

template <typename Operation> std::string captureLog(Operation operation) {
#if defined(LSB_LOCAL_RUNTIME_TESTING)
    sLog.clear();
    operation();
    return sLog;
#else
    int descriptors[2];
    assert(pipe(descriptors) == 0);
    int savedStderr = dup(STDERR_FILENO);
    assert(savedStderr >= 0 && dup2(descriptors[1], STDERR_FILENO) == STDERR_FILENO);
    close(descriptors[1]);
    operation();
    assert(dup2(savedStderr, STDERR_FILENO) == STDERR_FILENO);
    close(savedStderr);
    std::string result;
    char buffer[1024];
    ssize_t size;
    while ((size = read(descriptors[0], buffer, sizeof(buffer))) > 0) result.append(buffer, static_cast<size_t>(size));
    assert(size == 0);
    close(descriptors[0]);
    return result;
#endif
}

void testStrings() {
    char buffer[8];
    assert(lsb::zero(buffer, sizeof(buffer)) == buffer);
    for (char character : buffer) assert(character == 0);
    assert(lsb::copy(buffer, "abcdef", 6) == buffer);
    assert(lsb::length(buffer) == 6 && lsb::compare(buffer, "abcdef") == 0);
    assert(lsb::compare("a", "b") < 0 && lsb::compare("b", "a") > 0);
    assert(lsb::compare("\xFF", "\x7F") > 0);
    assert(lsb::copyString(buffer, "a long input", sizeof(buffer)) == 12);
    assert(std::strcmp(buffer, "a long ") == 0);
    assert(lsb::copyString(nullptr, "hello", 0) == 5);
    assert(lsb::copyString(buffer, "hello", 1) == 5 && buffer[0] == '\0');
}

void testSyscalls() {
#if defined(LSB_LOCAL_RUNTIME_TESTING)
    lsb::SyscallResult result = lsb::syscall(1234567, 10, 11, 12, 13, 14, 15, 16, 17);
    assert(result.error == ENOSYS && result.value == ENOSYS);
    assert(sLastArguments[0] == 1234567);
    for (unsigned i = 1; i != 9; ++i) assert(sLastArguments[i] == i + 9);
    assert(lsb::machTrap(-61, 1, 2, 3, 4, 5, 6, 7, 8) == 0 && sLastTrap == -61);
    for (unsigned i = 1; i != 9; ++i) assert(sLastArguments[i] == i);
#else
    lsb::SyscallResult result = lsb::syscall(SYS_getpid);
    assert(!result.error && result.value == static_cast<uintptr_t>(getpid()));
    result = lsb::syscall(SYS_close, static_cast<uintptr_t>(-1));
    assert(result.error == EBADF && result.value == EBADF);
    uint64_t identity = 0;
    assert(pthread_threadid_np(nullptr, &identity) == 0);
    result = lsb::syscall(SYS_thread_selfid);
    assert(!result.error && result.value == identity);
    assert(lsb::machTrap(-28) == static_cast<int64_t>(mach_task_self()));
#endif
}

void testErrnoAndLocks() {
    *lsb::errnoAddress() = 1234;
    pthread_mutex_t mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
    assert(lsb::pthread_mutex_unlock(&mutex) == EPERM);
    assert(lsb::pthread_mutex_lock(&mutex) == 0);
    assert(lsb::pthread_mutex_lock(&mutex) == 0);
    std::atomic<bool> attempted = false;
    std::atomic<bool> entered = false;
    std::thread waiter([&] {
        assert(*lsb::errnoAddress() == 0);
        *lsb::errnoAddress() = 4321;
        assert(lsb::pthread_mutex_unlock(&mutex) == EPERM);
        attempted.store(true);
        assert(lsb::pthread_mutex_lock(&mutex) == 0);
        entered.store(true);
        assert(*lsb::errnoAddress() == 4321);
        assert(lsb::pthread_mutex_unlock(&mutex) == 0);
    });
    while (!attempted.load()) std::this_thread::yield();
    assert(lsb::pthread_mutex_unlock(&mutex) == 0);
    assert(!entered.load());
    assert(lsb::pthread_mutex_unlock(&mutex) == 0);
    waiter.join();
    assert(entered.load() && *lsb::errnoAddress() == 1234);
    assert(lsb::pthread_mutex_unlock(&mutex) == EPERM);
    assert(lsb::pthread_mutex_lock(nullptr) == EINVAL);
    assert(lsb::pthread_mutex_unlock(nullptr) == EINVAL);

    unsigned protectedCounter = 0;
    std::vector<std::thread> workers;
    for (unsigned thread = 0; thread != 8; ++thread) workers.emplace_back([&, thread] {
        *lsb::errnoAddress() = 100 + thread;
        for (unsigned count = 0; count != 1000; ++count) {
            assert(lsb::pthread_mutex_lock(&mutex) == 0);
            ++protectedCounter;
            assert(lsb::pthread_mutex_unlock(&mutex) == 0);
        }
        assert(*lsb::errnoAddress() == static_cast<int>(100 + thread));
    });
    for (auto &worker : workers) worker.join();
    assert(protectedCounter == 8000 && *lsb::errnoAddress() == 1234);
}

void testLogging() {
    std::string actual = captureLog([] {
        logMessage("[%s] %.3s %c %p %d %+d %u %#x %#X %#o %%", "dyld", "abcdef", 'A', reinterpret_cast<void *>(0x1234),
                   -42, 42, 42U, 0x2aU, 0x2aU, 42U);
    });
    assert(actual == "[dyld] abc A 0x1234 -42 +42 42 0x2a 0X2A 052 %");
    actual = captureLog([] { logMessage("%lld %llu %zu %td %hhd %hhu %hd %hu", LLONG_MIN, ULLONG_MAX, size_t(7), ptrdiff_t(-8), -129, 257U, -32769, 65537U); });
    assert(actual == "-9223372036854775808 18446744073709551615 7 -8 127 1 32767 1");
    actual = captureLog([] { logMessage("%08x|%-5s|%*.*d|%#08x|%.0d|%#.0o|%.0s", 42U, "a", -6, 4, 12, 42U, 0, 0U, "abc"); });
    assert(actual == "0000002a|a    |0012  |0x00002a||0|");
    actual = captureLog([] { logMessage("%s %p % .3d %+.0d", static_cast<const char *>(nullptr), static_cast<void *>(nullptr), 7, 0); });
    assert(actual == "(null) 0x0  007 +");
    actual = captureLog([] { logMessage("bad %f later %d", 1.0, 2); });
    assert(actual == "bad [unsupported dyld log format: %f]");
    actual = captureLog([] { logMessage("%999999999999999999999d", 1); });
    assert(actual == "[unsupported dyld log format: %999999999999999999999d]");

    std::string large(2048, 'x');
#if defined(LSB_LOCAL_RUNTIME_TESTING)
    sInterruptedWrites = 3;
    sWriteLimit = 13;
#endif
    actual = captureLog([&] { logMessage("%s", large.c_str()); });
    assert(actual == large);
    assert(*lsb::errnoAddress() == 1234);
#if defined(LSB_LOCAL_RUNTIME_TESTING)
    sWriteFails = true;
    actual = captureLog([&] { logMessage("%s", large.c_str()); });
    assert(actual.empty());
    sWriteFails = false;
#endif
}

} // namespace

#if defined(LSB_LOCAL_RUNTIME_TESTING)
lsb::SyscallResult LSBTestSystemCall(uintptr_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                                   uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
    if (number == SYS_thread_selfid) {
        uint64_t identity;
        assert(pthread_threadid_np(nullptr, &identity) == 0);
        return {identity, 0};
    }
    if (number == SYS_write) {
        std::lock_guard<std::mutex> guard(sLogMutex);
        assert(a0 == 2);
        if (sInterruptedWrites) { --sInterruptedWrites; return {EINTR, EINTR}; }
        if (sWriteFails) return {EIO, EIO};
        size_t count = a2 < sWriteLimit ? a2 : sWriteLimit;
        sLog.append(reinterpret_cast<const char *>(a1), count);
        return {count, 0};
    }
    assert(number != SYS_exit);
    uintptr_t values[] = {number, a0, a1, a2, a3, a4, a5, a6, a7};
    std::memcpy(sLastArguments, values, sizeof(values));
    return {ENOSYS, ENOSYS};
}

int64_t LSBTestMachTrap(int64_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                       uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
    assert(number == -61);
    if (a0 == 0) { std::this_thread::yield(); return 0; }
    sLastTrap = number;
    uintptr_t values[] = {static_cast<uintptr_t>(number), a0, a1, a2, a3, a4, a5, a6, a7};
    std::memcpy(sLastArguments, values, sizeof(values));
    return 0;
}
#endif

int main() {
    testStrings();
    testSyscalls();
    testErrnoAndLocks();
    testLogging();
#if defined(LSB_LOCAL_RUNTIME_TESTING)
    std::puts("Local runtime mocked tests passed");
#else
    std::puts("Local runtime direct ARM64 syscall tests passed");
#endif
}
