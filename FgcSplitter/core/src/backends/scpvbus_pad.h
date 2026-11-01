#pragma once

#include "pad_backend.h"

namespace fgc {

class ScpVBusPadBus final : public IVirtualPadBus {
public:
    bool isAvailable() const override;
    FgcResult plugIn(std::unique_ptr<IVirtualPad>& out) override;
};

}  // namespace fgc
