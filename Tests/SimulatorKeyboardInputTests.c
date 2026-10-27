#include "../Host/SimulatorKeyboardInput.h"
#include <assert.h>
#include <stdio.h>

enum { Control = 1, Shift = 2, Option = 4, Command = 8 };
typedef struct { uint32_t usage; bool pressed; } KeyEvent;
typedef struct { KeyEvent events[64]; size_t count, attempts, failAt; int error; } Recorder;

static int record(void *context, uint32_t usage, bool pressed) {
    Recorder *recorder = context;
    if (recorder->attempts++ == recorder->failAt) return recorder->error;
    assert(recorder->count < sizeof(recorder->events) / sizeof(recorder->events[0]));
    recorder->events[recorder->count++] = (KeyEvent){usage, pressed};
    return 0;
}

static void clearRecorder(Recorder *recorder) { *recorder = (Recorder){.failAt = SIZE_MAX, .error = EAGAIN}; }

static void expect(const Recorder *recorder, size_t index, uint32_t usage, bool pressed) {
    assert(index < recorder->count && recorder->events[index].usage == usage && recorder->events[index].pressed == pressed);
}

static void expectReset(const LSBKeyboardInput *state) {
    assert(!state->synthesizedModifiers);
    for (size_t i = 0; i < sizeof(state->down) / sizeof(state->down[0]); ++i) assert(!state->down[i]);
}

static void synthesizedModifiersAndRepeat(void) {
    LSBKeyboardInput state = {0};
    Recorder recorder;
    clearRecorder(&recorder);
    uint16_t letter = 4;
    assert(!LSBKeyboardInputApply(&state, &letter, 1, true, Shift, record, &recorder));
    assert(recorder.count == 2 && state.synthesizedModifiers == Shift);
    expect(&recorder, 0, 0xe1, true);
    expect(&recorder, 1, 4, true);
    assert(state.down[4] && state.down[0xe1]);
    clearRecorder(&recorder);
    assert(!LSBKeyboardInputApply(&state, &letter, 1, true, Shift, record, &recorder));
    assert(!recorder.count); // UIKit repeated key-down must not create a second guest transition.
    assert(!LSBKeyboardInputApply(&state, &letter, 1, false, Shift, record, &recorder));
    assert(recorder.count == 1 && state.down[0xe1]);
    expect(&recorder, 0, 4, false);
    assert(!LSBKeyboardInputApply(&state, NULL, 0, false, 0, record, &recorder));
    assert(recorder.count == 2);
    expect(&recorder, 1, 0xe1, false);
    expectReset(&state);
    clearRecorder(&recorder);
    assert(!LSBKeyboardInputApply(&state, &letter, 1, false, 0, record, &recorder) && !recorder.count);

    assert(!LSBKeyboardInputApply(&state, &letter, 1, true, Control | Shift | Option | Command, record, &recorder));
    assert(recorder.count == 5 && state.synthesizedModifiers == 15);
    for (unsigned i = 0; i < 4; ++i) expect(&recorder, i, 0xe0 + i, true);
    expect(&recorder, 4, 4, true);
    clearRecorder(&recorder);
    assert(!LSBKeyboardInputApply(&state, &letter, 1, false, 0, record, &recorder));
    assert(recorder.count == 5);
    expect(&recorder, 0, 4, false);
    for (unsigned i = 0; i < 4; ++i) expect(&recorder, i + 1, 0xe0 + i, false);
    expectReset(&state);
}

static void explicitSidesAndOrdering(void) {
    LSBKeyboardInput state = {0};
    Recorder recorder;
    clearRecorder(&recorder);
    // Input order is deliberately ordinary-before-modifier, as NSSet iteration need not order modifier presses first.
    const uint16_t chord[] = {4, 0xe5, 0xe7};
    assert(!LSBKeyboardInputApply(&state, chord, 3, true, Shift | Command, record, &recorder));
    assert(recorder.count == 3 && !state.synthesizedModifiers && !state.down[0xe1] && !state.down[0xe3]);
    expect(&recorder, 0, 0xe5, true);
    expect(&recorder, 1, 0xe7, true);
    expect(&recorder, 2, 4, true);
    clearRecorder(&recorder);
    assert(!LSBKeyboardInputApply(&state, NULL, 0, true, 0, record, &recorder));
    assert(!recorder.count && state.down[0xe5] && state.down[0xe7]); // Aggregate flags cannot override explicit side identity.
    assert(!LSBKeyboardInputApply(&state, chord, 3, false, Shift | Command, record, &recorder));
    assert(recorder.count == 3); // Stale ending modifierFlags must not resynthesize just-released sides.
    expect(&recorder, 0, 4, false);
    expect(&recorder, 1, 0xe5, false);
    expect(&recorder, 2, 0xe7, false);
    expectReset(&state);

    clearRecorder(&recorder);
    assert(!LSBKeyboardInputApply(&state, NULL, 0, true, Shift, record, &recorder));
    uint16_t rightShift = 0xe5;
    clearRecorder(&recorder);
    assert(!LSBKeyboardInputApply(&state, &rightShift, 1, true, Shift, record, &recorder));
    assert(recorder.count == 2 && !state.synthesizedModifiers && !state.down[0xe1] && state.down[0xe5]);
    expect(&recorder, 0, 0xe5, true);
    expect(&recorder, 1, 0xe1, false);
    assert(!LSBKeyboardInputApply(&state, &rightShift, 1, false, Shift, record, &recorder));
    expectReset(&state);

    clearRecorder(&recorder);
    assert(!LSBKeyboardInputApply(&state, NULL, 0, true, Shift, record, &recorder));
    uint16_t leftShift = 0xe1;
    clearRecorder(&recorder);
    assert(!LSBKeyboardInputApply(&state, &leftShift, 1, true, Shift, record, &recorder));
    assert(!recorder.count && !state.synthesizedModifiers && state.down[0xe1]);
    assert(!LSBKeyboardInputApply(&state, &leftShift, 1, false, Shift, record, &recorder));
    assert(recorder.count == 1);
    expect(&recorder, 0, 0xe1, false);
    expectReset(&state);
}

static void partialFailureAndValidation(void) {
    LSBKeyboardInput state = {0};
    Recorder recorder;
    clearRecorder(&recorder);
    const uint16_t invalid[] = {0, 1, 2, 3, 0xe8, UINT16_MAX};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        uint16_t batch[] = {4, invalid[i]};
        assert(LSBKeyboardInputApply(&state, batch, 2, true, Shift, record, &recorder) == EINVAL);
    }
    assert(LSBKeyboardInputApply(NULL, NULL, 0, true, 0, record, &recorder) == EINVAL);
    assert(LSBKeyboardInputApply(&state, NULL, 1, true, 0, record, &recorder) == EINVAL);
    assert(LSBKeyboardInputApply(&state, NULL, 0, true, 0, NULL, &recorder) == EINVAL);
    assert(LSBKeyboardInputApply(&state, NULL, 0, true, 0x10, record, &recorder) == EINVAL);
    assert(!recorder.attempts);
    expectReset(&state);

    uint16_t letter = 4;
    recorder.failAt = 0;
    assert(LSBKeyboardInputApply(&state, &letter, 1, true, Shift, record, &recorder) == EAGAIN);
    assert(recorder.attempts == 1 && !recorder.count);
    expectReset(&state);
    clearRecorder(&recorder);
    recorder.failAt = 1;
    assert(LSBKeyboardInputApply(&state, &letter, 1, true, Shift, record, &recorder) == EAGAIN);
    assert(recorder.attempts == 2 && recorder.count == 1 && state.down[0xe1] && !state.down[4]);
    assert(state.synthesizedModifiers == Shift);
    // The UI must ask the server to release accepted keys first; clearing the local state does not pretend to send those releases.
    LSBKeyboardInputReset(&state);
    assert(recorder.count == 1);
    expectReset(&state);
    clearRecorder(&recorder);
    assert(!LSBKeyboardInputApply(&state, &letter, 1, true, 0, record, &recorder));
    clearRecorder(&recorder);
    recorder.failAt = 0;
    recorder.error = EPIPE;
    assert(LSBKeyboardInputApply(&state, &letter, 1, false, 0, record, &recorder) == EPIPE && state.down[4]);
    assert(recorder.attempts == 1 && !recorder.count);
    LSBKeyboardInputReset(&state);
    expectReset(&state);
}

int main(void) {
    synthesizedModifiersAndRepeat();
    explicitSidesAndOrdering();
    partialFailureAndValidation();
    puts("SimulatorKeyboardInputTests: modifier identity/order, focus recovery, repeat filtering and partial failure passed");
    return 0;
}
