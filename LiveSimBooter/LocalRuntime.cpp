// App-local implementation of the Darwin ARM64 calling convention and minimal C runtime operations.
// No host libSystem functions, thread-local-variable bootstrap, allocation, or dynamic initializers are used here.
#include "LocalRuntime.h"

#include <errno.h>
#include <limits.h>
#include <mach/thread_switch.h>
#include <sys/syscall.h>

#if defined(LSB_LOCAL_RUNTIME_TESTING)
lsb::SyscallResult LSBTestSystemCall(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
int64_t LSBTestMachTrap(int64_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#elif !defined(__APPLE__) || !defined(__arm64__)
#error The cache-independent runtime requires Darwin ARM64.
#endif

namespace lsb {

SyscallResult syscall(uintptr_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                      uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
#if defined(LSB_LOCAL_RUNTIME_TESTING)
    return LSBTestSystemCall(number, a0, a1, a2, a3, a4, a5, a6, a7);
#else
    register uintptr_t x0 __asm("x0") = a0;
    register uintptr_t x1 __asm("x1") = a1;
    register uintptr_t x2 __asm("x2") = a2;
    register uintptr_t x3 __asm("x3") = a3;
    register uintptr_t x4 __asm("x4") = a4;
    register uintptr_t x5 __asm("x5") = a5;
    register uintptr_t x6 __asm("x6") = a6;
    register uintptr_t x7 __asm("x7") = a7;
    register uintptr_t x16 __asm("x16") = number;
    uintptr_t error;
    // BSD traps return errno in x0 with carry set; successes may legitimately have a negative signed result.
    __asm volatile("svc #0x80\n\tcsel %9, x0, xzr, cs"
                   : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7), "+r"(x16), "=r"(error)
                   : : "cc", "memory");
    return {x0, static_cast<int>(error)};
#endif
}

int64_t machTrap(int64_t number, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3,
                 uintptr_t a4, uintptr_t a5, uintptr_t a6, uintptr_t a7) {
#if defined(LSB_LOCAL_RUNTIME_TESTING)
    return LSBTestMachTrap(number, a0, a1, a2, a3, a4, a5, a6, a7);
#else
    // Mach traps return their result directly, rather than using the BSD carry/errno convention.
    return static_cast<int64_t>(lsb::syscall(static_cast<uintptr_t>(number), a0, a1, a2, a3, a4, a5, a6, a7).value);
#endif
}

[[noreturn]] void exitProcess(int status) {
    for (;;) (void)lsb::syscall(SYS_exit, static_cast<uintptr_t>(status));
}

void *copy(void *destination, const void *source, size_t size) {
    auto output = static_cast<volatile unsigned char *>(destination);
    auto input = static_cast<const volatile unsigned char *>(source);
    for (size_t i = 0; i != size; ++i) output[i] = input[i];
    return destination;
}

void *zero(void *destination, size_t size) {
    auto output = static_cast<volatile unsigned char *>(destination);
    for (size_t i = 0; i != size; ++i) output[i] = 0;
    return destination;
}

size_t length(const char *string) {
    size_t size = 0;
    while (string[size]) ++size;
    return size;
}

int compare(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return static_cast<unsigned char>(*left) - static_cast<unsigned char>(*right);
}

size_t copyString(char *destination, const char *source, size_t capacity) {
    size_t size = length(source);
    if (capacity) {
        size_t copied = size < capacity - 1 ? size : capacity - 1;
        copy(destination, source, copied);
        destination[copied] = '\0';
    }
    return size;
}

namespace {

bool writeBytes(const char *bytes, size_t size) {
    while (size) {
        SyscallResult result = lsb::syscall(SYS_write, 2, reinterpret_cast<uintptr_t>(bytes), size);
        if (result.error == EINTR) continue;
        if (result.error || !result.value || result.value > size) return false;
        bytes += result.value;
        size -= result.value;
    }
    return true;
}

[[noreturn]] void runtimeFailure(const char *reason) {
    writeBytes(reason, length(reason));
    exitProcess(127);
}

uint64_t threadIdentity() {
    SyscallResult result = lsb::syscall(SYS_thread_selfid);
    if (result.error || !result.value) runtimeFailure("[dyld_sim] Unable to identify callback thread.\n");
    return result.value;
}

// Slots remain valid for the process lifetime, so errno pointers never dangle. Exhaustion is explicit, not a shared errno race.
// No pthread/TLV state is accessed, and thread_selfid does not allocate a Mach send right.
struct ErrnoSlot { uint64_t thread; int error; };
constexpr size_t kErrnoSlots = 4096;
ErrnoSlot sErrno[kErrnoSlots] = {};

struct MutexSlot { uintptr_t identity; uint64_t owner; uint32_t depth; };
constexpr size_t kMutexSlots = 512;
MutexSlot sMutexes[kMutexSlots] = {};

MutexSlot *findMutex(pthread_mutex_t *mutex, bool create) {
    uintptr_t identity = reinterpret_cast<uintptr_t>(mutex);
    if (!identity) return nullptr;
    size_t start = (identity >> 3) % kMutexSlots;
    for (size_t probe = 0; probe != kMutexSlots; ++probe) {
        MutexSlot &slot = sMutexes[(start + probe) % kMutexSlots];
        uintptr_t current = __atomic_load_n(&slot.identity, __ATOMIC_ACQUIRE);
        if (current == identity) return &slot;
        if (!current) {
            if (!create) return nullptr;
            if (__atomic_compare_exchange_n(&slot.identity, &current, identity, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return &slot;
            if (current == identity) return &slot;
        }
    }
    return nullptr;
}

class LogOutput {
public:
    void flush() {
        if (_used && _working) _working = writeBytes(_buffer, _used);
        _used = 0;
    }
    void put(char character) {
        if (!_working) return;
        _buffer[_used++] = character;
        if (_used == sizeof(_buffer)) flush();
    }
    void append(const char *string, size_t size) { for (size_t i = 0; i != size; ++i) put(string[i]); }
    void repeat(char character, unsigned count) { while (count--) put(character); }
private:
    char _buffer[512];
    size_t _used = 0;
    bool _working = true;
};

enum class IntegerSize { Normal, Char, Short, Long, LongLong, Size, PointerDifference, Max };

uint64_t unsignedArgument(va_list &arguments, IntegerSize size) {
    switch (size) {
        case IntegerSize::Char: return static_cast<unsigned char>(va_arg(arguments, unsigned));
        case IntegerSize::Short: return static_cast<unsigned short>(va_arg(arguments, unsigned));
        case IntegerSize::Long: return va_arg(arguments, unsigned long);
        case IntegerSize::LongLong: return va_arg(arguments, unsigned long long);
        case IntegerSize::Size: return va_arg(arguments, size_t);
        case IntegerSize::PointerDifference: return va_arg(arguments, uintptr_t);
        case IntegerSize::Max: return va_arg(arguments, uintmax_t);
        default: return va_arg(arguments, unsigned);
    }
}

int64_t signedArgument(va_list &arguments, IntegerSize size) {
    switch (size) {
        case IntegerSize::Char: return static_cast<signed char>(va_arg(arguments, int));
        case IntegerSize::Short: return static_cast<short>(va_arg(arguments, int));
        case IntegerSize::Long: return va_arg(arguments, long);
        case IntegerSize::LongLong: return va_arg(arguments, long long);
        case IntegerSize::Size: return va_arg(arguments, intptr_t);
        case IntegerSize::PointerDifference: return va_arg(arguments, ptrdiff_t);
        case IntegerSize::Max: return va_arg(arguments, intmax_t);
        default: return va_arg(arguments, int);
    }
}

// Bound pathological field widths without allocating or overflowing the formatter's counters.
constexpr int kMaximumField = 65536;
int fieldNumber(const char *&format) {
    int number = 0;
    while (*format >= '0' && *format <= '9') {
        if (number <= kMaximumField) number = number * 10 + (*format - '0');
        ++format;
    }
    return number;
}

void unsupportedFormat(LogOutput &output, const char *start, const char *end) {
    const char *message = "[unsupported dyld log format: ";
    output.append(message, length(message));
    output.append(start, static_cast<size_t>(end - start));
    output.put(']');
}

} // namespace

int *errnoAddress() {
    uint64_t identity = threadIdentity();
    size_t start = identity % kErrnoSlots;
    for (size_t probe = 0; probe != kErrnoSlots; ++probe) {
        ErrnoSlot &slot = sErrno[(start + probe) % kErrnoSlots];
        uint64_t current = __atomic_load_n(&slot.thread, __ATOMIC_ACQUIRE);
        if (current == identity) return &slot.error;
        if (!current) {
            if (__atomic_compare_exchange_n(&slot.thread, &current, identity, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return &slot.error;
            if (current == identity) return &slot.error;
        }
    }
    runtimeFailure("[dyld_sim] Callback errno thread-slot capacity exceeded.\n");
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    MutexSlot *slot = findMutex(mutex, true);
    if (!slot) return EAGAIN;
    uint64_t identity = threadIdentity();
    if (__atomic_load_n(&slot->owner, __ATOMIC_RELAXED) == identity) {
        if (slot->depth == UINT32_MAX) return EAGAIN;
        ++slot->depth;
        return 0;
    }
    unsigned spins = 0;
    for (;;) {
        uint64_t unowned = 0;
        if (__atomic_compare_exchange_n(&slot->owner, &unowned, identity, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            slot->depth = 1;
            return 0;
        }
        if (++spins == 64) {
            // XNU Mach trap 61 is thread_switch; temporarily depress this waiter to let the owner run.
            (void)machTrap(-61, 0, SWITCH_OPTION_DEPRESS, 1);
            spins = 0;
        } else {
#if defined(__arm64__)
            __asm volatile("yield" : : : "memory");
#endif
        }
    }
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    MutexSlot *slot = findMutex(mutex, false);
    if (!slot || __atomic_load_n(&slot->owner, __ATOMIC_RELAXED) != threadIdentity()) return EPERM;
    if (!--slot->depth) __atomic_store_n(&slot->owner, 0, __ATOMIC_RELEASE);
    return 0;
}

void vlog(const char *format, va_list arguments) {
    LogOutput output;
    if (!format) { output.append("(null log format)", 17); output.flush(); return; }
    va_list input;
    va_copy(input, arguments);
    while (*format) {
        if (*format != '%') { output.put(*format++); continue; }
        const char *start = format++;
        if (*format == '%') { output.put(*format++); continue; }
        bool left = false, plus = false, space = false, alternate = false, padded = false;
        for (;;) {
            if (*format == '-') left = true;
            else if (*format == '+') plus = true;
            else if (*format == ' ') space = true;
            else if (*format == '#') alternate = true;
            else if (*format == '0') padded = true;
            else break;
            ++format;
        }
        int width;
        if (*format == '*') {
            ++format;
            width = va_arg(input, int);
            if (width < 0) { left = true; width = width == INT_MIN ? kMaximumField + 1 : -width; }
        } else width = fieldNumber(format);
        int precision = -1;
        if (*format == '.') {
            ++format;
            if (*format == '*') { ++format; precision = va_arg(input, int); }
            else precision = fieldNumber(format);
        }
        IntegerSize size = IntegerSize::Normal;
        if (*format == 'h') { ++format; size = IntegerSize::Short; if (*format == 'h') { ++format; size = IntegerSize::Char; } }
        else if (*format == 'l') { ++format; size = IntegerSize::Long; if (*format == 'l') { ++format; size = IntegerSize::LongLong; } }
        else if (*format == 'q') { ++format; size = IntegerSize::LongLong; }
        else if (*format == 'z') { ++format; size = IntegerSize::Size; }
        else if (*format == 't') { ++format; size = IntegerSize::PointerDifference; }
        else if (*format == 'j') { ++format; size = IntegerSize::Max; }
        char conversion = *format;
        if (conversion) ++format;
        if (width > kMaximumField || precision > kMaximumField) { unsupportedFormat(output, start, format); break; }
        if ((conversion == 's' || conversion == 'c') && size == IntegerSize::Normal) {
            char character = 0;
            const char *text;
            size_t count = 0;
            if (conversion == 'c') { character = static_cast<char>(va_arg(input, int)); text = &character; count = 1; }
            else {
                text = va_arg(input, const char *);
                if (!text) text = "(null)";
                while ((precision < 0 || count < static_cast<size_t>(precision)) && text[count]) ++count;
            }
            unsigned padding = static_cast<size_t>(width) > count ? static_cast<unsigned>(width - count) : 0;
            if (!left) output.repeat(' ', padding);
            output.append(text, count);
            if (left) output.repeat(' ', padding);
            continue;
        }
        bool isSigned = conversion == 'd' || conversion == 'i';
        bool pointer = conversion == 'p' && size == IntegerSize::Normal;
        unsigned base = (conversion == 'x' || conversion == 'X' || pointer) ? 16 : conversion == 'o' ? 8 : 10;
        if (!isSigned && !pointer && conversion != 'u' && conversion != 'o' && conversion != 'x' && conversion != 'X') {
            unsupportedFormat(output, start, format);
            break;
        }
        uint64_t value;
        char sign = 0;
        if (isSigned) {
            int64_t signedValue = signedArgument(input, size);
            if (signedValue < 0) { sign = '-'; value = 0 - static_cast<uint64_t>(signedValue); }
            else { sign = plus ? '+' : space ? ' ' : 0; value = static_cast<uint64_t>(signedValue); }
        } else value = pointer ? reinterpret_cast<uintptr_t>(va_arg(input, void *)) : unsignedArgument(input, size);
        const char *alphabet = conversion == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        char digits[64];
        unsigned count = 0;
        if (value || precision != 0 || pointer) {
            do { digits[count++] = alphabet[value % base]; value /= base; } while (value);
        }
        unsigned leadingZeros = precision > static_cast<int>(count) ? precision - count : 0;
        bool hexadecimalPrefix = pointer || (alternate && base == 16 && count && !(count == 1 && digits[0] == '0'));
        if (alternate && base == 8 && (!count || digits[count - 1] != '0') && !leadingZeros) ++leadingZeros;
        unsigned prefix = (sign ? 1 : 0) + (hexadecimalPrefix ? 2 : 0);
        unsigned total = count + leadingZeros + prefix;
        unsigned padding = static_cast<unsigned>(width) > total ? width - total : 0;
        if (padded && !left && precision < 0) { leadingZeros += padding; padding = 0; }
        if (!left) output.repeat(' ', padding);
        if (sign) output.put(sign);
        if (hexadecimalPrefix) { output.put('0'); output.put(conversion == 'X' ? 'X' : 'x'); }
        output.repeat('0', leadingZeros);
        while (count) output.put(digits[--count]);
        if (left) output.repeat(' ', padding);
    }
    va_end(input);
    output.flush();
}

} // namespace lsb
