#include "controller_limit.h"

namespace fgc {

FgcControllerLimit computeControllerLimit(uint32_t xinputConnected, uint32_t activePads, uint32_t settledPads,
                                          uint32_t requested) {
    FgcControllerLimit limit{};
    limit.struct_size = sizeof(limit);
    limit.physical_connected = xinputConnected > settledPads ? xinputConnected - settledPads : 0;
    limit.virtual_active = activePads;
    limit.virtual_requested = requested;
    limit.allowed = limit.physical_connected + activePads + requested <= FGC_MAX_XINPUT_CONTROLLERS ? 1 : 0;
    return limit;
}

}  // namespace fgc
