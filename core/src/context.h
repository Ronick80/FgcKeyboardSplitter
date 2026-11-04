#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "backends/controller_counter.h"
#include "backends/keyboard_backend.h"
#include "backends/pad_backend.h"
#include "engine/event_queue.h"
#include "engine/mapping_session.h"
#include "profiles/profile_store.h"

struct FgcContext {
    std::unique_ptr<fgc::IKeyboardInput> keyboard;
    std::unique_ptr<fgc::IVirtualPadBus> padBus;
    std::unique_ptr<fgc::IControllerCounter> counter;
    fgc::ProfileStore profiles{fgc::ProfileStore::defaultRoot()};

    // Guards the control-plane state below. Mapping threads never take it.
    std::mutex controlLock;
    std::vector<fgc::KeyboardDevice> keyboards;
    std::map<std::string, fgc::KeyArray> mappings;  // per keyboard hardware ID; absent means default
    std::unique_ptr<fgc::IIdentifySession> identify;  // identify mode or watch mode, whichever is active
    bool watching = false;  // watch mode: identify is rebuilt as keyboards start, stop, appear or disappear
    std::chrono::steady_clock::time_point watchRescanAt{};
    size_t watchedSessionCount = 0;  // sessions that existed when the watch was last rebuilt

    std::atomic<uint32_t> runningSessions{0};
    fgc::EventQueue events;

    // Checks once a second that running keyboards are still plugged in and clears finished sessions.
    std::mutex watchdogMutex;
    std::condition_variable watchdogWake;
    bool watchdogStop = false;
    std::thread watchdog;

    // Declared last so sessions stop before the counter and backends go away.
    std::map<std::string, std::unique_ptr<fgc::MappingSession>> sessions;
};
