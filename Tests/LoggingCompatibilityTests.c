#include "../Guest/LoggingCompatibility.h"
#include <assert.h>
#include <errno.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>

#ifdef LSB_LOGGING_INTERPOSE_TESTING

int main(void) {
    // No configuration or constructor call is required before this first query.
    errno = EDOM;
    xpc_object_t value = xpc_copy_entitlement_for_token(LSB_LOGGING_DISABLE_MACH_PORTS_ENTITLEMENT, NULL);
    assert(value == XPC_BOOL_TRUE && errno == EDOM);
    xpc_release(value);
    audit_token_t audit;
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    assert(!task_info(mach_task_self(), TASK_AUDIT_TOKEN, (task_info_t)&audit, &count));
    value = xpc_copy_entitlement_for_token(LSB_LOGGING_DISABLE_MACH_PORTS_ENTITLEMENT, &audit);
    assert(value != XPC_BOOL_TRUE); // This unsigned fixture has no actual logging entitlement, even for an explicit self token.
    if (value) xpc_release(value);
    value = xpc_copy_entitlement_for_token("com.kdt.LiveSimBooter.test.nonexistent-entitlement", NULL);
    assert(!value);
    value = xpc_copy_entitlement_for_token(NULL, NULL);
    assert(!value || xpc_get_type(value) == XPC_TYPE_DICTIONARY); // A request for the complete dictionary is never synthesized.
    if (value) xpc_release(value);
    puts("LoggingCompatibility interpose: exact self policy, explicit audit validation and unrelated queries passed");
    return 0;
}

#else

static unsigned originalCalls, booleanCalls;
static const char *seenKey;
static const audit_token_t *seenToken;
static xpc_object_t originalResult;

static xpc_object_t originalQuery(const char *key, const audit_token_t *token) {
    ++originalCalls;
    seenKey = key;
    seenToken = token;
    errno = EACCES;
    return originalResult;
}

static xpc_object_t createBoolean(bool value) {
    ++booleanCalls;
    assert(value);
    return xpc_bool_create(value);
}

#define LSB_LOGGING_TESTING
#define xpc_copy_entitlement_for_token originalQuery
#define xpc_bool_create createBoolean
#include "../Guest/LoggingCompatibility.c"
#undef xpc_copy_entitlement_for_token
#undef xpc_bool_create

static void passthrough(const char *key, const audit_token_t *token) {
    originalCalls = booleanCalls = 0;
    errno = EDOM;
    xpc_object_t result = LSBLoggingCopyEntitlementForToken(key, token);
    assert(result == originalResult && originalCalls == 1 && !booleanCalls && seenKey == key && seenToken == token && errno == EACCES);
}

int main(void) {
    originalResult = xpc_bool_create(false);
    const char key[] = LSB_LOGGING_DISABLE_MACH_PORTS_ENTITLEMENT;
#ifdef LSB_GUEST_RUNTIME
    errno = EDOM;
    xpc_object_t result = LSBLoggingCopyEntitlementForToken(key, NULL);
    assert(result == XPC_BOOL_TRUE && !originalCalls && booleanCalls == 1 && errno == EDOM);
    xpc_release(result);
    char prefix[sizeof(key)];
    for (size_t length = 0; length < sizeof(key) - 1; ++length) {
        memcpy(prefix, key, length);
        prefix[length] = '\0';
        passthrough(prefix, NULL);
    }
#else
    // Compiling this source outside the guest build must never enable the override.
    passthrough(key, NULL);
    (void)createBoolean;
#endif
    passthrough(NULL, NULL);
    passthrough("com.apple.private.disable-log-mach-ports.extra", NULL);
    passthrough("com.apple.private.disable-log-mach-port", NULL);
    passthrough("com.apple.private.Disable-log-mach-ports", NULL);
    passthrough("com.apple.private.task_for_pid-allow", NULL);
    audit_token_t target = {{0}};
    passthrough(key, &target);
    target.val[5] = 1234;
    target.val[7] = 12;
    passthrough(key, &target);
    passthrough(key, (const audit_token_t *)(uintptr_t)1); // Target validation belongs to libxpc, never dereference or replace it here.
    passthrough((const char *)(uintptr_t)1, &target); // Even key inspection is skipped for explicit targets.
    xpc_release(originalResult);
    originalResult = NULL;
    passthrough("missing", NULL);
    passthrough(key, &target);
    originalResult = xpc_bool_create(true);
    passthrough("original-true", NULL);
    xpc_release(originalResult);
    puts("LoggingCompatibility: exact key/self scope, early-call policy, native build and original error forwarding passed");
    return 0;
}

#endif
