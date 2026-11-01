#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "backends/keyboard_backend.h"
#include "backends/pad_backend.h"

struct FgcContext {
    std::unique_ptr<fgc::IKeyboardInput> keyboard;
    std::unique_ptr<fgc::IVirtualPadBus> padBus;

    // Guards the control-plane state below. Mapping threads never take it.
    std::mutex controlLock;
    std::vector<fgc::KeyboardDevice> keyboards;
};
