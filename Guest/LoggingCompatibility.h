#pragma once

#include <mach/message.h>
#include <xpc/xpc.h>

#define LSB_LOGGING_DISABLE_MACH_PORTS_ENTITLEMENT "com.apple.private.disable-log-mach-ports"

// Private libxpc C API used by the stock simulator libtrace guard; NULL token means the calling process.
extern xpc_object_t xpc_copy_entitlement_for_token(const char *key, const audit_token_t *token);

// Guest-only logging policy while diagnosticd is absent. Other keys and all explicit targets retain native validation.
// This changes no signed entitlement, broker snapshot, host process, or access to native services.
xpc_object_t LSBLoggingCopyEntitlementForToken(const char *key, const audit_token_t *token);
