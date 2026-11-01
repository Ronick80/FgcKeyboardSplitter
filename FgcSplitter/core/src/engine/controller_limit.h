#pragma once

#include <cstdint>

#include "fgc_core.h"

namespace fgc {

// xinputConnected counts every controller XInput reports, including this library's own pads that Windows
// has already enumerated (settledPads of them). activePads counts all pads this library holds.
FgcControllerLimit computeControllerLimit(uint32_t xinputConnected, uint32_t activePads, uint32_t settledPads,
                                          uint32_t requested);

}  // namespace fgc
