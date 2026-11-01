#pragma once

#include <cstdint>

#include "backends/pad_backend.h"

namespace fgc {

// heldInputs has bit i set while FgcXboxInput i is held. Opposing directions are resolved
// here: left+right cancels to neutral and up+down resolves to up (leverless convention).
PadState computePadState(uint32_t heldInputs) noexcept;

}  // namespace fgc
