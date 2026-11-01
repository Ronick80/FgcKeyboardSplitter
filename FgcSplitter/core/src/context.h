#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "backends/keyboard_backend.h"
#include "backends/pad_backend.h"
#include "engine/mapping_session.h"

struct FgcContext {
    std::unique_ptr<fgc::IKeyboardInput> keyboard;
    std::unique_ptr<fgc::IVirtualPadBus> padBus;

    // Guards the control-plane state below. Mapping threads never take it.
    std::mutex controlLock;
    std::vector<fgc::KeyboardDevice> keyboards;
    std::map<std::string, fgc::KeyArray> mappings;  // per keyboard hardware ID; absent means default
    std::unique_ptr<fgc::IIdentifySession> identify;

    std::atomic<uint32_t> runningSessions{0};

    // Declared last so sessions stop before the counter and backends go away.
    std::map<std::string, std::unique_ptr<fgc::MappingSession>> sessions;
};
