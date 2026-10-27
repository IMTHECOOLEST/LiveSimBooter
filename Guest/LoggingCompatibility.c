#include "LoggingCompatibility.h"

xpc_object_t LSBLoggingCopyEntitlementForToken(const char *key, const audit_token_t *token) {
#ifdef LSB_GUEST_RUNTIME
    if (!token && key) {
        static const char expected[] = LSB_LOGGING_DISABLE_MACH_PORTS_ENTITLEMENT;
        size_t index = 0;
        while (expected[index] && key[index] == expected[index]) ++index;
        if (key[index] == expected[index]) {
            // Sim26Booter PATCHING.md (2025-09-11) forces this libtrace guard while diagnosticd is absent:
            // https://github.com/khanhduytran0/Sim26Booter/blob/main/PATCHING.md#2025-09-11
            // The matching libtrace calls xpc_copy_entitlement_for_token(key, NULL) at +0x7330 before caching the result.
            // This can precede guest TLS/constructors: no environment, errno, allocation, path, or audit inspection here.
            // The pinned libxpc Boolean factory is a leaf returning its immutable singleton with copy ownership.
            return xpc_bool_create(true);
        }
    }
#endif
    __attribute__((musttail)) return xpc_copy_entitlement_for_token(key, token);
}

#if defined(LSB_GUEST_RUNTIME) && !defined(LSB_LOGGING_TESTING)
__attribute__((used, section("__DATA,__interpose"))) static const struct { const void *replacement, *original; } interposition = {
    (const void *)&LSBLoggingCopyEntitlementForToken, (const void *)&xpc_copy_entitlement_for_token
};
#endif
