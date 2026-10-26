#pragma once

#include <stddef.h>

// Parse only the iOS 15+ DER entitlement format; generate XML for simulator csops operation 7 callers.
// Both buffers include their big-endian code-signing blob headers. Success returns malloc-owned bytes; free them with free().
// Failure returns an errno value and clears the outputs. This runs in the native host before publishing a guest snapshot.
int LSBCopyXMLFromDEREntitlements(const void *der, size_t derSize, void **xml, size_t *xmlSize);
