#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

// App-resident primitives for callbacks made after the host shared cache has been detached.
// Compile the implementation without builtins, stack protection, exceptions, or RTTI.
namespace lsb {

struct SyscallResult {
    uintptr_t value;
    int error;
};

SyscallResult syscall(uintptr_t number, uintptr_t a0 = 0, uintptr_t a1 = 0, uintptr_t a2 = 0, uintptr_t a3 = 0,
                      uintptr_t a4 = 0, uintptr_t a5 = 0, uintptr_t a6 = 0, uintptr_t a7 = 0);
int64_t machTrap(int64_t number, uintptr_t a0 = 0, uintptr_t a1 = 0, uintptr_t a2 = 0, uintptr_t a3 = 0,
                 uintptr_t a4 = 0, uintptr_t a5 = 0, uintptr_t a6 = 0, uintptr_t a7 = 0);
int *errnoAddress();
void *copy(void *destination, const void *source, size_t size);
void *zero(void *destination, size_t size);
size_t length(const char *string);
int compare(const char *left, const char *right);
size_t copyString(char *destination, const char *source, size_t capacity);
[[noreturn]] void exitProcess(int status);
void vlog(const char *format, va_list arguments);

// A recursive bridge for dyld's locks, keyed by mutex identity without using host pthread object storage.
// This is not a general replacement for process-shared, robust, or host-owned pthread mutexes.
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

} // namespace lsb
