#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fgc_core.h"

namespace fgc {

enum class DriverState { NotInstalled, RebootRequired, Installed };

struct KeyboardDevice {
    std::string hardwareId;
    std::string displayName;
};

// keyCode uses the FgcMapping encoding (set-1 scan code, 0x100 for E0-prefixed keys).
struct KeyEvent {
    uint16_t keyCode = 0;
    bool down = false;
    int64_t timestampQpc = 0;
};

enum class WaitResult { Event, Interrupted, DeviceLost };

// One captured keyboard. Every event it returns is consumed, so other apps never see it.
// Destroying the session releases the keyboard.
class IKeyboardSession {
public:
    virtual ~IKeyboardSession() = default;

    // Hot path: blocks without polling and must not allocate.
    virtual WaitResult wait(KeyEvent& out) noexcept = 0;

    // Wakes a blocked wait() so its thread can exit.
    virtual void interrupt() noexcept = 0;
};

class IKeyboardInput {
public:
    virtual ~IKeyboardInput() = default;

    virtual DriverState driverState() const = 0;
    virtual FgcResult enumerate(std::vector<KeyboardDevice>& out) = 0;
    virtual FgcResult open(const std::string& hardwareId, std::unique_ptr<IKeyboardSession>& out) = 0;
};

}  // namespace fgc
