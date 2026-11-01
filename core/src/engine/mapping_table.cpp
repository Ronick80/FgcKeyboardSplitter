#include "mapping_table.h"

namespace fgc {

MappingTable MappingTable::fromKeys(const KeyArray& keyForInput) {
    MappingTable table;
    for (size_t input = 0; input < keyForInput.size(); ++input) {
        const uint16_t key = keyForInput[input];
        if (key != FGC_KEY_UNBOUND && key < kKeyCodeSpace) {
            table.inputsForKey[key] |= 1u << input;
        }
    }
    return table;
}

KeyArray defaultKeys() {
    KeyArray keys{};

    keys[FGC_INPUT_A] = 0x24;  // J
    keys[FGC_INPUT_B] = 0x25;  // K
    keys[FGC_INPUT_X] = 0x16;  // U
    keys[FGC_INPUT_Y] = 0x17;  // I
    keys[FGC_INPUT_LEFT_BUMPER] = 0x18;   // O
    keys[FGC_INPUT_RIGHT_BUMPER] = 0x19;  // P
    keys[FGC_INPUT_LEFT_TRIGGER] = 0x26;   // L
    keys[FGC_INPUT_RIGHT_TRIGGER] = 0x27;  // ;
    keys[FGC_INPUT_START] = 0x1C;  // Enter
    keys[FGC_INPUT_BACK] = 0x0F;   // Tab

    keys[FGC_INPUT_DPAD_UP] = 0x11;     // W
    keys[FGC_INPUT_DPAD_DOWN] = 0x1F;   // S
    keys[FGC_INPUT_DPAD_LEFT] = 0x1E;   // A
    keys[FGC_INPUT_DPAD_RIGHT] = 0x20;  // D

    keys[FGC_INPUT_LEFT_STICK_UP] = FGC_KEY_EXTENDED_FLAG | 0x48;     // Up arrow
    keys[FGC_INPUT_LEFT_STICK_DOWN] = FGC_KEY_EXTENDED_FLAG | 0x50;   // Down arrow
    keys[FGC_INPUT_LEFT_STICK_LEFT] = FGC_KEY_EXTENDED_FLAG | 0x4B;   // Left arrow
    keys[FGC_INPUT_LEFT_STICK_RIGHT] = FGC_KEY_EXTENDED_FLAG | 0x4D;  // Right arrow

    return keys;
}

}  // namespace fgc
