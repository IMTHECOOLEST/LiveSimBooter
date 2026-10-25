#pragma once

#include <mach/message.h>
#include <stddef.h>
#include <stdint.h>

// CoreSimulator 1171.7's little-endian wire ABI. Copy received bytes into these structures before reading fields;
// envelope and payload addresses are not necessarily naturally aligned. Unused bytes in each item remain zero.
typedef struct {
    mach_msg_header_t header;
    mach_msg_body_t body;
} FBWireMessagePrefix;

#pragma pack(push, 1)
typedef struct {
    uint64_t magic;
    uint8_t session[16];
    uint32_t itemCount;
} FBWireEnvelope;

typedef struct {
    uint64_t magic;
    uint32_t type, stride;
} FBWireItemHeader;

typedef struct {
    FBWireItemHeader header;
    uint8_t payload[248];
} FBWireItem;

typedef struct {
    uint32_t eventDescriptorIndex, reserved;
    char name[64];
} FBWireCheckIn;

typedef struct { uint8_t identifier[16]; } FBWireSession;

typedef struct {
    char displayUID[64], displayName[64], deviceName[64];
    uint64_t supportedSurfaceFlags, displayFlags;
    uint32_t supportedPresentationModes, deviceWidth, deviceHeight, canvasWidth, canvasHeight, displayID, displayType, dotPitch;
    uint16_t maximumLayerCount, maximumSwapchainCount;
} FBWireDisplayProperties;

typedef struct {
    uint32_t modeID, width, height, scale, hdr, pixelFormat, colorSpace, refreshHundredths;
    uint8_t preferred, native, current;
} FBWireDisplayMode;

typedef struct {
    uint32_t width, height, displayID, swapchainID, pixelFormat, colorSpace, hdr, presentationMode;
} FBWireSwapchain;

typedef struct { uint32_t x, y, width, height; } FBWireRectangle;

typedef struct {
    uint64_t operationID, desiredNanoseconds, flags;
    FBWireRectangle source, destination;
    uint32_t displayID, swapchainID, surfaceID, surfaceDescriptorIndex, fenceDescriptorIndex, layer;
} FBWirePresent;

typedef struct { uint32_t displayID, swapchainID; } FBWireCancel;

typedef struct {
    uint64_t operationID, desiredNanoseconds, completedNanoseconds;
    uint32_t swapchainID;
} FBWireCompletion;

typedef struct {
    char description[140];
    uint64_t domain;
    int64_t code;
} FBWireError;
#pragma pack(pop)

enum { FBWireTrailingPadding = 20, FBWireEmptyMessageSize = sizeof(FBWireMessagePrefix) + sizeof(FBWireEnvelope) + FBWireTrailingPadding };

_Static_assert(sizeof(mach_msg_header_t) == 24 && sizeof(mach_msg_port_descriptor_t) == 12, "framebuffer Mach descriptor ABI");
_Static_assert(sizeof(FBWireMessagePrefix) == 28 && offsetof(FBWireMessagePrefix, body) == 24, "framebuffer message prefix ABI");
_Static_assert(sizeof(FBWireEnvelope) == 28 && offsetof(FBWireEnvelope, session) == 8 && offsetof(FBWireEnvelope, itemCount) == 24,
               "framebuffer envelope ABI");
_Static_assert(sizeof(FBWireItemHeader) == 16 && offsetof(FBWireItemHeader, type) == 8 && offsetof(FBWireItemHeader, stride) == 12,
               "framebuffer item header ABI");
_Static_assert(sizeof(FBWireItem) == 264 && offsetof(FBWireItem, payload) == 16, "framebuffer item ABI");
_Static_assert(sizeof(FBWireCheckIn) == 72 && offsetof(FBWireCheckIn, reserved) == 4 && offsetof(FBWireCheckIn, name) == 8,
               "framebuffer check-in ABI");
_Static_assert(sizeof(FBWireSession) == 16, "framebuffer session ABI");
_Static_assert(sizeof(FBWireDisplayProperties) == 244 && offsetof(FBWireDisplayProperties, displayName) == 64 &&
               offsetof(FBWireDisplayProperties, deviceName) == 128 && offsetof(FBWireDisplayProperties, supportedSurfaceFlags) == 192 &&
               offsetof(FBWireDisplayProperties, displayFlags) == 200 && offsetof(FBWireDisplayProperties, supportedPresentationModes) == 208 &&
               offsetof(FBWireDisplayProperties, deviceWidth) == 212 && offsetof(FBWireDisplayProperties, deviceHeight) == 216 &&
               offsetof(FBWireDisplayProperties, canvasWidth) == 220 && offsetof(FBWireDisplayProperties, canvasHeight) == 224 &&
               offsetof(FBWireDisplayProperties, displayID) == 228 && offsetof(FBWireDisplayProperties, displayType) == 232 &&
               offsetof(FBWireDisplayProperties, dotPitch) == 236 && offsetof(FBWireDisplayProperties, maximumLayerCount) == 240 &&
               offsetof(FBWireDisplayProperties, maximumSwapchainCount) == 242, "framebuffer display properties ABI");
_Static_assert(sizeof(FBWireDisplayMode) == 35 && offsetof(FBWireDisplayMode, width) == 4 && offsetof(FBWireDisplayMode, height) == 8 &&
               offsetof(FBWireDisplayMode, scale) == 12 && offsetof(FBWireDisplayMode, hdr) == 16 &&
               offsetof(FBWireDisplayMode, pixelFormat) == 20 && offsetof(FBWireDisplayMode, colorSpace) == 24 &&
               offsetof(FBWireDisplayMode, refreshHundredths) == 28 && offsetof(FBWireDisplayMode, preferred) == 32 &&
               offsetof(FBWireDisplayMode, native) == 33 && offsetof(FBWireDisplayMode, current) == 34, "framebuffer display mode ABI");
_Static_assert(sizeof(FBWireSwapchain) == 32 && offsetof(FBWireSwapchain, height) == 4 && offsetof(FBWireSwapchain, displayID) == 8 &&
               offsetof(FBWireSwapchain, swapchainID) == 12 && offsetof(FBWireSwapchain, pixelFormat) == 16 &&
               offsetof(FBWireSwapchain, colorSpace) == 20 && offsetof(FBWireSwapchain, hdr) == 24 &&
               offsetof(FBWireSwapchain, presentationMode) == 28, "framebuffer swapchain ABI");
_Static_assert(sizeof(FBWireRectangle) == 16 && offsetof(FBWireRectangle, y) == 4 && offsetof(FBWireRectangle, width) == 8 &&
               offsetof(FBWireRectangle, height) == 12, "framebuffer rectangle ABI");
_Static_assert(sizeof(FBWirePresent) == 80 && offsetof(FBWirePresent, desiredNanoseconds) == 8 && offsetof(FBWirePresent, flags) == 16 &&
               offsetof(FBWirePresent, source) == 24 && offsetof(FBWirePresent, destination) == 40 && offsetof(FBWirePresent, displayID) == 56 &&
               offsetof(FBWirePresent, swapchainID) == 60 && offsetof(FBWirePresent, surfaceID) == 64 &&
               offsetof(FBWirePresent, surfaceDescriptorIndex) == 68 && offsetof(FBWirePresent, fenceDescriptorIndex) == 72 &&
               offsetof(FBWirePresent, layer) == 76, "framebuffer presentation ABI");
_Static_assert(sizeof(FBWireCancel) == 8 && offsetof(FBWireCancel, swapchainID) == 4, "framebuffer cancellation ABI");
_Static_assert(sizeof(FBWireCompletion) == 28 && offsetof(FBWireCompletion, desiredNanoseconds) == 8 &&
               offsetof(FBWireCompletion, completedNanoseconds) == 16 && offsetof(FBWireCompletion, swapchainID) == 24,
               "framebuffer completion ABI");
_Static_assert(sizeof(FBWireError) == 156 && offsetof(FBWireError, domain) == 140 && offsetof(FBWireError, code) == 148,
               "framebuffer error ABI");
