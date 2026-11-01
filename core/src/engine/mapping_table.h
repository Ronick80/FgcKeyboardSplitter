#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "fgc_core.h"

namespace fgc {

// Key codes use the FgcMapping encoding: scan code, plus 0x100 when E0-prefixed.
constexpr size_t kKeyCodeSpace = 0x200;

using KeyArray = std::array<uint16_t, FGC_INPUT_COUNT>;

// Reverse lookup so a key event resolves to its inputs with a single array read.
struct MappingTable {
    std::array<uint32_t, kKeyCodeSpace> inputsForKey{};  // bit i = FgcXboxInput i
    uint32_t version = 0;  // lets the mapping thread notice a replaced table

    static MappingTable fromKeys(const KeyArray& keyForInput);
};

KeyArray defaultKeys();

}  // namespace fgc
