#pragma once

#include "keyboard_backend.h"

namespace fgc {

class InterceptionKeyboardInput final : public IKeyboardInput {
public:
    DriverState driverState() const override;
    FgcResult enumerate(std::vector<KeyboardDevice>& out) override;
    FgcResult open(const std::string& hardwareId, std::unique_ptr<IKeyboardSession>& out) override;
    FgcResult beginIdentify(std::unique_ptr<IIdentifySession>& out) override;

private:
    struct Known {
        int deviceNumber = 0;  // Interception keyboard number, 1..10
        std::string rawHardwareId;
        std::string hardwareId;  // raw id, with #N appended for identical keyboards after the first
    };

    // Last scan; a keyboard held by one of our own sessions can't be reopened, so it is reported from here.
    std::vector<Known> known_;
};

}  // namespace fgc
