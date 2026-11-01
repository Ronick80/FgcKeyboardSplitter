#pragma once

#include <cstdint>

namespace fgc {

// Counts the XInput controller slots Windows currently reports as connected.
class IControllerCounter {
public:
    virtual ~IControllerCounter() = default;

    virtual uint32_t connected() const = 0;
};

class XInputControllerCounter final : public IControllerCounter {
public:
    uint32_t connected() const override;
};

}  // namespace fgc
