#pragma once

#include "keyboard_backend.h"

namespace fgc {

class InterceptionKeyboardInput final : public IKeyboardInput {
public:
    DriverState driverState() const override;
    FgcResult enumerate(std::vector<KeyboardDevice>& out) override;
    FgcResult open(const std::string& hardwareId, std::unique_ptr<IKeyboardSession>& out) override;
};

}  // namespace fgc
