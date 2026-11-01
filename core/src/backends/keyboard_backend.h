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

    // False once the keyboard has been unplugged or replaced. Callable from any thread while wait() blocks.
    virtual bool connected() const noexcept = 0;

    // Tells Windows these keys are up. Windows never saw a captured key's press, but a key that was already
    // down when capture began would otherwise stay stuck down once its release is swallowed.
    virtual void releaseKeys(const uint16_t* keyCodes, size_t count) noexcept = 0;
};

// Reports which keyboard produces the next key press. Key presses still reach Windows unchanged.
class IIdentifySession {
public:
    virtual ~IIdentifySession() = default;

    // Non-blocking. True once a key press was seen, with that keyboard's hardware ID.
    virtual bool poll(std::string& hardwareId) = 0;
};

class IKeyboardInput {
public:
    virtual ~IKeyboardInput() = default;

    virtual DriverState driverState() const = 0;
    virtual FgcResult enumerate(std::vector<KeyboardDevice>& out) = 0;
    virtual FgcResult open(const std::string& hardwareId, std::unique_ptr<IKeyboardSession>& out) = 0;
    virtual FgcResult beginIdentify(std::unique_ptr<IIdentifySession>& out) = 0;
};

}  // namespace fgc
