#pragma once

#include "pad_backend.h"

namespace fgc {

// Neutralizes and unplugs every pad this process still holds. For crash handlers; normal shutdown
// destroys the pads instead.
void releaseAllPadsNow() noexcept;

class ScpVBusPadBus final : public IVirtualPadBus {
public:
    bool isAvailable() const override;
    FgcResult plugIn(std::unique_ptr<IVirtualPad>& out) override;
};

}  // namespace fgc
