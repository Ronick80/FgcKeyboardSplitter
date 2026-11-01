#include "pad_state.h"

namespace fgc {
namespace {

constexpr uint16_t kDpadUp = 0x0001;
constexpr uint16_t kDpadDown = 0x0002;
constexpr uint16_t kDpadLeft = 0x0004;
constexpr uint16_t kDpadRight = 0x0008;

struct ButtonBit {
    FgcXboxInput input;
    uint16_t mask;
};

constexpr ButtonBit kButtons[] = {
    {FGC_INPUT_START, 0x0010},
    {FGC_INPUT_BACK, 0x0020},
    {FGC_INPUT_LEFT_STICK_CLICK, 0x0040},
    {FGC_INPUT_RIGHT_STICK_CLICK, 0x0080},
    {FGC_INPUT_LEFT_BUMPER, 0x0100},
    {FGC_INPUT_RIGHT_BUMPER, 0x0200},
    {FGC_INPUT_GUIDE, 0x0400},
    {FGC_INPUT_A, 0x1000},
    {FGC_INPUT_B, 0x2000},
    {FGC_INPUT_X, 0x4000},
    {FGC_INPUT_Y, 0x8000},
};

constexpr int16_t kAxisMax = 32767;
constexpr int16_t kAxisMin = -32768;

bool isHeld(uint32_t inputs, FgcXboxInput input) {
    return ((inputs >> input) & 1u) != 0;
}

int16_t horizontalAxis(bool left, bool right) {
    if (left == right) {
        return 0;
    }
    return right ? kAxisMax : kAxisMin;
}

int16_t verticalAxis(bool up, bool down) {
    if (up) {
        return kAxisMax;
    }
    return down ? kAxisMin : 0;
}

}  // namespace

PadState computePadState(uint32_t heldInputs) noexcept {
    PadState state;

    for (const ButtonBit& button : kButtons) {
        if (isHeld(heldInputs, button.input)) {
            state.buttons = static_cast<uint16_t>(state.buttons | button.mask);
        }
    }

    const bool dpadLeft = isHeld(heldInputs, FGC_INPUT_DPAD_LEFT);
    const bool dpadRight = isHeld(heldInputs, FGC_INPUT_DPAD_RIGHT);
    const bool dpadUp = isHeld(heldInputs, FGC_INPUT_DPAD_UP);
    const bool dpadDown = isHeld(heldInputs, FGC_INPUT_DPAD_DOWN);
    if (dpadLeft != dpadRight) {
        state.buttons = static_cast<uint16_t>(state.buttons | (dpadLeft ? kDpadLeft : kDpadRight));
    }
    if (dpadUp || dpadDown) {
        state.buttons = static_cast<uint16_t>(state.buttons | (dpadUp ? kDpadUp : kDpadDown));
    }

    state.leftTrigger = isHeld(heldInputs, FGC_INPUT_LEFT_TRIGGER) ? 255 : 0;
    state.rightTrigger = isHeld(heldInputs, FGC_INPUT_RIGHT_TRIGGER) ? 255 : 0;

    state.thumbLX = horizontalAxis(isHeld(heldInputs, FGC_INPUT_LEFT_STICK_LEFT),
                                   isHeld(heldInputs, FGC_INPUT_LEFT_STICK_RIGHT));
    state.thumbLY =
        verticalAxis(isHeld(heldInputs, FGC_INPUT_LEFT_STICK_UP), isHeld(heldInputs, FGC_INPUT_LEFT_STICK_DOWN));
    state.thumbRX = horizontalAxis(isHeld(heldInputs, FGC_INPUT_RIGHT_STICK_LEFT),
                                   isHeld(heldInputs, FGC_INPUT_RIGHT_STICK_RIGHT));
    state.thumbRY =
        verticalAxis(isHeld(heldInputs, FGC_INPUT_RIGHT_STICK_UP), isHeld(heldInputs, FGC_INPUT_RIGHT_STICK_DOWN));

    return state;
}

}  // namespace fgc
