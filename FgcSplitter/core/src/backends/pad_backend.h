#pragma once

#include <cstdint>
#include <memory>

#include "fgc_core.h"

namespace fgc {

// Layout matches XINPUT_GAMEPAD.
struct PadState {
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t thumbLX = 0;
    int16_t thumbLY = 0;
    int16_t thumbRX = 0;
    int16_t thumbRY = 0;
};

// One virtual Xbox 360 controller. Destroying it unplugs it.
class IVirtualPad {
public:
    virtual ~IVirtualPad() = default;

    // Hot path: must not allocate or lock.
    virtual FgcResult submit(const PadState& state) noexcept = 0;

    // Sends the neutral state.
    virtual void reset() noexcept = 0;
};

class IVirtualPadBus {
public:
    virtual ~IVirtualPadBus() = default;

    virtual bool isAvailable() const = 0;
    virtual FgcResult plugIn(std::unique_ptr<IVirtualPad>& out) = 0;
};

}  // namespace fgc
