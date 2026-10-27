#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <errno.h>
#include <string.h>

// USB keyboard page 7. Modifier bits follow E0..E3 (Control, Shift, Option, Command); E4..E7 are their right variants.
typedef struct {
    bool down[0xe8];
    uint8_t synthesizedModifiers;
} LSBKeyboardInput;
typedef int (*LSBKeyboardInputSend)(void *context, uint32_t usage, bool pressed);

static inline void LSBKeyboardInputReset(LSBKeyboardInput *state) { memset(state, 0, sizeof(*state)); }

static inline int LSBKeyboardInputEmit(LSBKeyboardInput *state, uint16_t usage, bool pressed, LSBKeyboardInputSend send, void *context) {
    if (state->down[usage] == pressed) return 0;
    int error = send(context, usage, pressed);
    if (error) return error;
    state->down[usage] = pressed;
    if (!pressed && usage >= 0xe0 && usage <= 0xe3) state->synthesizedModifiers &= ~(1u << (usage - 0xe0));
    return 0;
}

// UIKit can report a modifier already held when this view gains focus only through aggregate modifierFlags.
// Preserve known left/right presses and synthesize a left press only when neither side has been observed.
static inline int LSBKeyboardInputModifiers(LSBKeyboardInput *state, uint8_t modifiers, LSBKeyboardInputSend send, void *context) {
    for (unsigned bit = 0; bit < 4; ++bit) {
        uint8_t flag = 1u << bit;
        uint16_t left = 0xe0 + bit, right = 0xe4 + bit;
        int error = 0;
        if ((state->synthesizedModifiers & flag) && (!(modifiers & flag) || state->down[right]))
            error = LSBKeyboardInputEmit(state, left, false, send, context);
        else if ((modifiers & flag) && !state->down[left] && !state->down[right]) {
            error = LSBKeyboardInputEmit(state, left, true, send, context);
            if (!error) state->synthesizedModifiers |= flag;
        }
        if (error) return error;
    }
    return 0;
}

// Batch modifiers before ordinary key downs, and release ordinary keys before modifiers on key up.
// On any error the caller must release the server's accepted keys, then reset this state; partial batches are never retried here.
static inline int LSBKeyboardInputApply(LSBKeyboardInput *state, const uint16_t *usages, size_t count, bool pressed,
                                       uint8_t modifiers, LSBKeyboardInputSend send, void *context) {
    if (!state || (!usages && count) || !send || (modifiers & ~0xfu)) return EINVAL;
    for (size_t i = 0; i < count; ++i) if (usages[i] < 4 || usages[i] > 0xe7) return EINVAL;
    for (unsigned pass = 0; pass < 2; ++pass) {
        bool modifierPass = pressed ? pass == 0 : pass == 1;
        for (size_t i = 0; i < count; ++i) {
            uint16_t usage = usages[i];
            if ((usage >= 0xe0) != modifierPass) continue;
            int error = LSBKeyboardInputEmit(state, usage, pressed, send, context);
            if (error) return error;
            if (usage >= 0xe0) {
                unsigned bit = (usage - 0xe0) & 3;
                // An actual left press supersedes the synthesized one without an up/down transition.
                if (pressed && usage < 0xe4) state->synthesizedModifiers &= ~(1u << bit);
                if (pressed) modifiers |= 1u << bit;
                else if (!state->down[0xe0 + bit] && !state->down[0xe4 + bit]) modifiers &= ~(1u << bit);
            }
        }
        if ((pressed && pass == 0) || (!pressed && pass == 1)) {
            int error = LSBKeyboardInputModifiers(state, modifiers, send, context);
            if (error) return error;
        }
    }
    return 0;
}
