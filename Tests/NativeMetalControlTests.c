#include "../ProcessSupport/NativeMetalControl.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static audit_token_t tokenFor(unsigned value) {
    audit_token_t token = {{0}};
    for (unsigned i = 0; i < 8; ++i) token.val[i] = value ^ (0x1020304u * i);
    token.val[0] = value;
    token.val[5] = value + 1000;
    return token;
}

static void assertCleared(const LSBNativeMetalAuthorization *authorization) {
    const unsigned char *bytes = (const void *)authorization;
    for (size_t i = 0; i < sizeof(*authorization); ++i) assert(!bytes[i]);
}

static void assertReadError(const LSBNativeMetalControl *control, int expected) {
    LSBNativeMetalAuthorization authorization;
    memset(&authorization, 0xa5, sizeof(authorization));
    assert(LSBNativeMetalControlRead(control, &authorization) == expected);
    assertCleared(&authorization);
}

static void basicAndMalformedTests(void) {
    LSBNativeMetalControl *control = malloc(LSB_NATIVE_METAL_CONTROL_SIZE);
    assert(control);
    assert(LSBNativeMetalControlInitialize(NULL, LSB_NATIVE_METAL_CONTROL_SIZE) == EINVAL);
    assert(LSBNativeMetalControlInitialize(control, LSB_NATIVE_METAL_CONTROL_SIZE - 1) == EINVAL);
    assert(LSBNativeMetalControlInitialize((void *)((char *)control + 1), LSB_NATIVE_METAL_CONTROL_SIZE) == EINVAL);
    assert(!LSBNativeMetalControlInitialize(control, LSB_NATIVE_METAL_CONTROL_SIZE));
    assert(!LSBNativeMetalControlValidate(control, LSB_NATIVE_METAL_CONTROL_SIZE));
    LSBNativeMetalAuthorization authorization;
    assert(!LSBNativeMetalControlRead(control, &authorization));
    assertCleared(&authorization);
    audit_token_t token = tokenFor(17), invalid = token;
    assert(!LSBNativeMetalControlPublish(control, &token));
    assert(!LSBNativeMetalControlRead(control, &authorization));
    assert(authorization.authorized && !authorization.publicAccess && authorization.generation == 1 &&
           !memcmp(&authorization.audit, &token, sizeof(token)));
    invalid.val[5] = 0;
    assert(LSBNativeMetalControlPublish(control, &invalid) == EINVAL);
    invalid.val[5] = UINT32_MAX;
    assert(LSBNativeMetalControlPublish(control, &invalid) == EINVAL);
    assert(!LSBNativeMetalControlRead(control, &authorization));
    assert(authorization.authorized && authorization.generation == 1 && !memcmp(&authorization.audit, &token, sizeof(token)));
    assert(!LSBNativeMetalControlPublish(control, NULL));
    assert(!LSBNativeMetalControlRead(control, &authorization));
    assert(!authorization.authorized && authorization.generation == 2);
    audit_token_t zero = {{0}};
    assert(!memcmp(&authorization.audit, &zero, sizeof(zero)));

    // Exercise fixed ABI fields and both ends of the reserved tail, using atomics even for deliberately malformed records.
    _Atomic(uint32_t) *words = (void *)control;
    unsigned malformedWords[] = {0, 1, 2, 3, 7, 16, 1023};
    for (unsigned i = 0; i < sizeof(malformedWords) / sizeof(malformedWords[0]); ++i) {
        unsigned index = malformedWords[i];
        uint32_t original = atomic_exchange(&words[index], 0xdeadc0de);
        assert(LSBNativeMetalControlValidate(control, LSB_NATIVE_METAL_CONTROL_SIZE) == EPROTO);
        assertReadError(control, EPROTO);
        assert(LSBNativeMetalControlPublish(control, &token) == EPROTO);
        assert(LSBNativeMetalControlPublishPublic(control) == EPROTO);
        atomic_store(&words[index], original);
    }
    atomic_store(&words[6], 3); // Invalid authorization discriminator.
    assertReadError(control, EPROTO);
    atomic_store(&words[6], 1);
    assertReadError(control, EPROTO); // An authorized token cannot have PID zero.
    atomic_store(&words[6], 0);
    atomic_store(&words[8], 1);
    assertReadError(control, EPROTO); // Revocation must clear every audit word.
    atomic_store(&words[8], 0);
    _Atomic(uint64_t) *sequence = (void *)((char *)control + 16);
    atomic_store(sequence, 7);
    assertReadError(control, EAGAIN); // Permanently odd input returns after the bounded retry count.
    assert(LSBNativeMetalControlPublish(control, &token) == EBUSY);
    atomic_store(sequence, UINT64_MAX - 1);
    assert(LSBNativeMetalControlPublish(control, &token) == EOVERFLOW);
    assertReadError(control, EOVERFLOW);
    assert(LSBNativeMetalControlPublish(control, NULL) == EOVERFLOW);
    assert(LSBNativeMetalControlPublishPublic(control) == EOVERFLOW);
    assertReadError(NULL, EINVAL);
    assert(LSBNativeMetalControlRead(control, NULL) == EINVAL);
    free(control);
}

static void publicAuthorizationTests(void) {
    LSBNativeMetalControl *control = malloc(LSB_NATIVE_METAL_CONTROL_SIZE);
    assert(control && !LSBNativeMetalControlInitialize(control, LSB_NATIVE_METAL_CONTROL_SIZE));
    assert(LSBNativeMetalControlPublishPublic(NULL) == EINVAL);
    audit_token_t token = tokenFor(123), zero = {{0}};
    assert(!LSBNativeMetalControlPublish(control, &token));
    assert(!LSBNativeMetalControlPublishPublic(control));
    LSBNativeMetalAuthorization authorization;
    assert(!LSBNativeMetalControlRead(control, &authorization));
    assert(authorization.authorized && authorization.publicAccess && authorization.generation == 2);
    assert(!memcmp(&authorization.audit, &zero, sizeof(zero))); // Public publication clears a previous owner's complete identity.
    _Atomic(uint32_t) *words = (void *)control;
    for (unsigned index = 8; index < 16; ++index) {
        atomic_store(&words[index], 123);
        assertReadError(control, EPROTO); // Public access cannot carry a residual or forged owner token.
        atomic_store(&words[index], 0);
    }
    uint32_t version = atomic_exchange(&words[1], 1);
    assertReadError(control, EPROTO); // A version-1 helper cannot interpret the public discriminator.
    atomic_store(&words[1], version);
    assert(!LSBNativeMetalControlPublish(control, NULL));
    assert(!LSBNativeMetalControlRead(control, &authorization));
    assert(!authorization.authorized && !authorization.publicAccess && authorization.generation == 3);
    assert(!memcmp(&authorization.audit, &zero, sizeof(zero)));
    assert(!LSBNativeMetalControlPublishPublic(control));
    assert(!LSBNativeMetalControlPublish(control, &token));
    assert(!LSBNativeMetalControlRead(control, &authorization));
    assert(authorization.authorized && !authorization.publicAccess && authorization.generation == 5);
    assert(!memcmp(&authorization.audit, &token, sizeof(token)));
    _Atomic(uint64_t) *sequence = (void *)((char *)control + 16);
    atomic_store(sequence, UINT64_MAX - 1);
    assert(LSBNativeMetalControlPublishPublic(control) == EOVERFLOW);
    assert(!atomic_load(&words[6]));
    for (unsigned index = 8; index < 16; ++index) assert(!atomic_load(&words[index]));
    assertReadError(control, EOVERFLOW);
    free(control);
}

typedef struct {
    LSBNativeMetalControl *control;
    atomic_uint ready, accepted;
    atomic_bool start, done;
} ConcurrentTest;

static void *readPublications(void *context) {
    ConcurrentTest *test = context;
    atomic_fetch_add(&test->ready, 1);
    while (!atomic_load(&test->start)) sched_yield();
    uint64_t generation = 0;
    do {
        LSBNativeMetalAuthorization authorization;
        int error = LSBNativeMetalControlRead(test->control, &authorization);
        if (error) { assert(error == EAGAIN); assertCleared(&authorization); continue; }
        assert(authorization.generation >= generation);
        generation = authorization.generation;
        if (authorization.authorized && !authorization.publicAccess) {
            audit_token_t expected = tokenFor(authorization.audit.val[0]);
            assert(!memcmp(&authorization.audit, &expected, sizeof(expected)));
        } else {
            audit_token_t zero = {{0}};
            assert(!memcmp(&authorization.audit, &zero, sizeof(zero)));
            assert(!authorization.publicAccess || authorization.authorized);
        }
        atomic_fetch_add(&test->accepted, 1);
    } while (!atomic_load(&test->done));
    LSBNativeMetalAuthorization final;
    assert(!LSBNativeMetalControlRead(test->control, &final) && !final.authorized);
    return NULL;
}

static void concurrentPublicationTests(void) {
    ConcurrentTest test = {.control = malloc(LSB_NATIVE_METAL_CONTROL_SIZE)};
    assert(test.control && !LSBNativeMetalControlInitialize(test.control, LSB_NATIVE_METAL_CONTROL_SIZE));
    pthread_t readers[4];
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_create(&readers[i], NULL, readPublications, &test));
    while (atomic_load(&test.ready) != 4) sched_yield();
    atomic_store(&test.start, true);
    for (unsigned i = 1; i <= 30000; ++i) {
        audit_token_t token = tokenFor(i);
        assert(!LSBNativeMetalControlPublish(test.control, &token));
        if (!(i % 3)) assert(!LSBNativeMetalControlPublish(test.control, NULL));
        if (!(i % 5)) assert(!LSBNativeMetalControlPublishPublic(test.control));
    }
    assert(!LSBNativeMetalControlPublish(test.control, NULL));
    atomic_store(&test.done, true);
    for (unsigned i = 0; i < 4; ++i) assert(!pthread_join(readers[i], NULL));
    assert(atomic_load(&test.accepted));
    free(test.control);
}

static void mappingAndProcessTests(void) {
    const char *directory = getenv("TMPDIR");
    if (!directory) directory = "/tmp";
    LSBNativeMetalControl *writer = (void *)1;
    int readerFD = 99;
    assert(LSBNativeMetalControlCreate("relative", &writer, &readerFD) == EINVAL && !writer && readerFD == -1);
    assert(!LSBNativeMetalControlCreate(directory, &writer, &readerFD));
    assert(writer && readerFD >= 0 && (fcntl(readerFD, F_GETFL) & O_ACCMODE) == O_RDONLY && (fcntl(readerFD, F_GETFD) & FD_CLOEXEC));
    struct stat info;
    assert(!fstat(readerFD, &info) && S_ISREG(info.st_mode) && !info.st_nlink && info.st_size == LSB_NATIVE_METAL_CONTROL_SIZE);
    assert((info.st_mode & 0777) == 0600);
    assert(mmap(NULL, LSB_NATIVE_METAL_CONTROL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, readerFD, 0) == MAP_FAILED && errno == EACCES);
    const LSBNativeMetalControl *reader = (void *)1;
    assert(LSBNativeMetalControlImport(-1, &reader) == EBADF && !reader);
    assert(!LSBNativeMetalControlImport(readerFD, &reader) && reader != writer);
    audit_token_t expected = tokenFor(123);
    assert(!LSBNativeMetalControlPublish(writer, &expected));
    LSBNativeMetalAuthorization authorization;
    assert(!LSBNativeMetalControlRead(reader, &authorization) && !memcmp(&authorization.audit, &expected, sizeof(expected)));

    int toChild[2], toParent[2];
    assert(!pipe(toChild) && !pipe(toParent));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(toChild[1]); close(toParent[0]);
        LSBNativeMetalControlUnmap(writer); // Only the separately imported read-only mapping remains in the child.
        const LSBNativeMetalControl *imported;
        assert(!LSBNativeMetalControlImport(readerFD, &imported));
        close(readerFD);
        char signal;
        assert(read(toChild[0], &signal, 1) == 1);
        assert(!LSBNativeMetalControlRead(imported, &authorization) && authorization.authorized);
        expected = tokenFor(456);
        assert(!memcmp(&authorization.audit, &expected, sizeof(expected)));
        assert(write(toParent[1], "a", 1) == 1);
        assert(read(toChild[0], &signal, 1) == 1);
        assert(!LSBNativeMetalControlRead(imported, &authorization) && authorization.authorized && authorization.publicAccess);
        audit_token_t zero = {{0}};
        assert(!memcmp(&authorization.audit, &zero, sizeof(zero)));
        assert(write(toParent[1], "p", 1) == 1);
        assert(read(toChild[0], &signal, 1) == 1);
        assert(!LSBNativeMetalControlRead(imported, &authorization) && !authorization.authorized);
        LSBNativeMetalControlUnmap(imported);
        _exit(0);
    }
    close(toChild[0]); close(toParent[1]);
    expected = tokenFor(456);
    assert(!LSBNativeMetalControlPublish(writer, &expected));
    assert(write(toChild[1], "a", 1) == 1);
    char signal;
    assert(read(toParent[0], &signal, 1) == 1);
    assert(!LSBNativeMetalControlPublishPublic(writer));
    assert(write(toChild[1], "p", 1) == 1);
    assert(read(toParent[0], &signal, 1) == 1);
    assert(!LSBNativeMetalControlPublish(writer, NULL));
    assert(write(toChild[1], "r", 1) == 1);
    close(toChild[1]); close(toParent[0]);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    LSBNativeMetalControlUnmap(reader);
    LSBNativeMetalControlUnmap(writer);
    close(readerFD);

    char path[1024];
    assert(snprintf(path, sizeof(path), "%s/lsb-metal-invalid.XXXXXX", directory) < (int)sizeof(path));
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(LSBNativeMetalControlImport(fd, &reader) == EACCES && !reader);
    int readonly = open(path, O_RDONLY);
    assert(readonly >= 0 && !unlink(path));
    assert(LSBNativeMetalControlImport(readonly, &reader) == EPROTO && !reader); // Wrong file size.
    assert(!ftruncate(fd, LSB_NATIVE_METAL_CONTROL_SIZE));
    assert(LSBNativeMetalControlImport(readonly, &reader) == EPROTO && !reader); // Missing header.
    close(readonly); close(fd);
    fd = open(directory, O_RDONLY);
    assert(fd >= 0 && LSBNativeMetalControlImport(fd, &reader) == EPROTO && !reader);
    close(fd);
    LSBNativeMetalControlUnmap(NULL);
}

int main(void) {
    alarm(45);
    basicAndMalformedTests();
    publicAuthorizationTests();
    mappingAndProcessTests(); // Fork before the thread stress test; no test-owned worker thread survives into the child.
    concurrentPublicationTests();
    puts("NativeMetalControl: dedicated/public publication and revocation, bounded failure, malformed ABI, read-only import and process sharing passed");
    return 0;
}
