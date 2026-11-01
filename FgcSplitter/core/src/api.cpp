#include "fgc_core.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "backends/interception_keyboard.h"
#include "backends/scpvbus_pad.h"
#include "context.h"
#include "engine/controller_limit.h"

namespace {

template <typename Fn>
FgcResult guarded(Fn&& fn) noexcept {
    try {
        return fn();
    } catch (...) {
        return FGC_ERR_INTERNAL;
    }
}

template <typename T>
bool validStruct(const T* ptr) {
    return ptr != nullptr && ptr->struct_size >= sizeof(T);
}

void copyTruncated(char* destination, size_t capacity, const std::string& source) {
    strncpy_s(destination, capacity, source.c_str(), _TRUNCATE);
}

FgcInterceptionState toPublic(fgc::DriverState state) {
    switch (state) {
        case fgc::DriverState::Installed:
            return FGC_INTERCEPTION_INSTALLED;
        case fgc::DriverState::RebootRequired:
            return FGC_INTERCEPTION_REBOOT_REQUIRED;
        case fgc::DriverState::NotInstalled:
            break;
    }
    return FGC_INTERCEPTION_NOT_INSTALLED;
}

// Caller holds the control lock. A keyboard with no mapping yet starts from its last-used profile.
fgc::KeyArray currentKeys(FgcContext& context, const std::string& hardwareId) {
    const auto stored = context.mappings.find(hardwareId);
    if (stored != context.mappings.end()) {
        return stored->second;
    }

    fgc::KeyArray keys = fgc::defaultKeys();
    std::string lastUsed;
    if (context.profiles.lastUsed(hardwareId, lastUsed)) {
        fgc::KeyArray loaded;
        if (context.profiles.load(hardwareId, lastUsed, loaded) == FGC_OK) {
            keys = loaded;
        }
    }
    context.mappings[hardwareId] = keys;
    return keys;
}

// Caller holds the control lock.
void applyKeys(FgcContext& context, const std::string& hardwareId, const fgc::KeyArray& keys) {
    context.mappings[hardwareId] = keys;
    const auto session = context.sessions.find(hardwareId);
    if (session != context.sessions.end() && !session->second->finished()) {
        session->second->setMapping(keys);
    }
}

FgcResult copyName(const std::string& name, char* out, uint32_t size) {
    if (name.size() + 1 > size) {
        return FGC_ERR_BUFFER_TOO_SMALL;
    }
    std::memcpy(out, name.c_str(), name.size() + 1);
    return FGC_OK;
}

// Windows needs a moment to list a freshly plugged pad, so a young pad is not yet subtracted from the
// XInput count; that errs toward refusing a start rather than allowing a fifth controller.
constexpr std::chrono::seconds kPadSettleTime{3};

// Caller holds the control lock.
FgcControllerLimit limitFor(const FgcContext& context, uint32_t requested) {
    const auto now = std::chrono::steady_clock::now();
    uint32_t settled = 0;
    for (const auto& entry : context.sessions) {
        if (now - entry.second->startedAt() >= kPadSettleTime) {
            ++settled;
        }
    }
    return fgc::computeControllerLimit(context.counter->connected(), static_cast<uint32_t>(context.sessions.size()),
                                       settled, requested);
}

uint64_t ticksToNanoseconds(uint64_t ticks) {
    static const uint64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return static_cast<uint64_t>(value.QuadPart);
    }();
    return ticks * 1'000'000'000ull / frequency;
}

// False when a key code is outside the range a mapping can hold.
bool toKeys(const FgcMapping& mapping, fgc::KeyArray& keys) {
    for (size_t i = 0; i < keys.size(); ++i) {
        if (mapping.key_for_input[i] >= fgc::kKeyCodeSpace) {
            return false;
        }
        keys[i] = mapping.key_for_input[i];
    }
    return true;
}

}  // namespace

extern "C" {

FGC_API uint32_t fgc_get_api_version(void) {
    return FGC_API_VERSION;
}

FGC_API FgcResult fgc_create(FgcContext** out_context) {
    if (!out_context) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    *out_context = nullptr;
    return guarded([&] {
        auto context = std::make_unique<FgcContext>();
        context->keyboard = std::make_unique<fgc::InterceptionKeyboardInput>();
        context->padBus = std::make_unique<fgc::ScpVBusPadBus>();
        context->counter = std::make_unique<fgc::XInputControllerCounter>();
        *out_context = context.release();
        return FGC_OK;
    });
}

FGC_API void fgc_destroy(FgcContext* context) {
    delete context;
}

FGC_API FgcResult fgc_get_driver_status(FgcContext* context, FgcDriverStatus* out_status) {
    if (!context || !validStruct(out_status)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        out_status->interception = toPublic(context->keyboard->driverState());
        out_status->scpvbus_installed = context->padBus->isAvailable() ? 1 : 0;
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_refresh_keyboards(FgcContext* context) {
    if (!context) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        std::vector<fgc::KeyboardDevice> found;
        const FgcResult result = context->keyboard->enumerate(found);
        if (result == FGC_OK) {
            context->keyboards = std::move(found);
        }
        return result;
    });
}

FGC_API FgcResult fgc_get_keyboard_count(FgcContext* context, uint32_t* out_count) {
    if (!context || !out_count) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        *out_count = static_cast<uint32_t>(context->keyboards.size());
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_get_keyboard(FgcContext* context, uint32_t index, FgcKeyboardInfo* out_info) {
    if (!context || !validStruct(out_info)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        if (index >= context->keyboards.size()) {
            return FGC_ERR_NOT_FOUND;
        }
        const fgc::KeyboardDevice& device = context->keyboards[index];
        copyTruncated(out_info->hardware_id, FGC_HARDWARE_ID_MAX, device.hardwareId);
        copyTruncated(out_info->display_name, FGC_DISPLAY_NAME_MAX, device.displayName);
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_identify_begin(FgcContext* context) {
    if (!context) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        context->identify.reset();
        return context->keyboard->beginIdentify(context->identify);
    });
}

FGC_API FgcResult fgc_identify_cancel(FgcContext* context) {
    if (!context) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        context->identify.reset();
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_identify_poll(FgcContext* context, int32_t* out_index) {
    if (!context || !out_index) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    *out_index = -1;
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        std::string hardwareId;
        if (!context->identify || !context->identify->poll(hardwareId)) {
            return FGC_OK;
        }

        const auto indexOf = [&] {
            for (size_t i = 0; i < context->keyboards.size(); ++i) {
                if (context->keyboards[i].hardwareId == hardwareId) {
                    return static_cast<int32_t>(i);
                }
            }
            return int32_t{-1};
        };
        *out_index = indexOf();
        if (*out_index < 0) {
            // The cached list predates this keyboard.
            std::vector<fgc::KeyboardDevice> found;
            if (context->keyboard->enumerate(found) == FGC_OK) {
                context->keyboards = std::move(found);
                *out_index = indexOf();
            }
        }
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_get_default_mapping(FgcMapping* out_mapping) {
    if (!validStruct(out_mapping)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    const fgc::KeyArray keys = fgc::defaultKeys();
    std::copy(keys.begin(), keys.end(), out_mapping->key_for_input);
    return FGC_OK;
}

FGC_API FgcResult fgc_get_mapping(FgcContext* context, const char* hardware_id, FgcMapping* out_mapping) {
    if (!context || !hardware_id || !validStruct(out_mapping)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        const fgc::KeyArray keys = currentKeys(*context, hardware_id);
        std::copy(keys.begin(), keys.end(), out_mapping->key_for_input);
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_set_mapping(FgcContext* context, const char* hardware_id, const FgcMapping* mapping) {
    if (!context || !hardware_id || !validStruct(mapping)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    fgc::KeyArray keys;
    if (!toKeys(*mapping, keys)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        applyKeys(*context, hardware_id, keys);
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_profile_count(FgcContext* context, const char* hardware_id, uint32_t* out_count) {
    if (!context || !hardware_id || !out_count) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        std::vector<std::string> names;
        const FgcResult result = context->profiles.list(hardware_id, names);
        if (result == FGC_OK) {
            *out_count = static_cast<uint32_t>(names.size());
        }
        return result;
    });
}

FGC_API FgcResult fgc_profile_get_name(FgcContext* context, const char* hardware_id, uint32_t index,
                                       char* out_name, uint32_t out_name_size) {
    if (!context || !hardware_id || !out_name || out_name_size == 0) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        std::vector<std::string> names;
        const FgcResult result = context->profiles.list(hardware_id, names);
        if (result != FGC_OK) {
            return result;
        }
        if (index >= names.size()) {
            return FGC_ERR_NOT_FOUND;
        }
        return copyName(names[index], out_name, out_name_size);
    });
}

FGC_API FgcResult fgc_profile_load(FgcContext* context, const char* hardware_id, const char* name,
                                   FgcMapping* out_mapping) {
    if (!context || !hardware_id || !name || (out_mapping && !validStruct(out_mapping))) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        fgc::KeyArray keys;
        const FgcResult result = context->profiles.load(hardware_id, name, keys);
        if (result != FGC_OK) {
            return result;
        }
        applyKeys(*context, hardware_id, keys);
        context->profiles.setLastUsed(hardware_id, name);
        if (out_mapping) {
            std::copy(keys.begin(), keys.end(), out_mapping->key_for_input);
        }
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_profile_save(FgcContext* context, const char* hardware_id, const char* name,
                                   const FgcMapping* mapping) {
    if (!context || !hardware_id || !name || !validStruct(mapping)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    fgc::KeyArray keys;
    if (!toKeys(*mapping, keys)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        return context->profiles.save(hardware_id, name, keys);
    });
}

FGC_API FgcResult fgc_profile_rename(FgcContext* context, const char* hardware_id, const char* old_name,
                                     const char* new_name) {
    if (!context || !hardware_id || !old_name || !new_name) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        return context->profiles.rename(hardware_id, old_name, new_name);
    });
}

FGC_API FgcResult fgc_profile_delete(FgcContext* context, const char* hardware_id, const char* name) {
    if (!context || !hardware_id || !name) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        return context->profiles.remove(hardware_id, name);
    });
}

FGC_API FgcResult fgc_profile_get_last_used(FgcContext* context, const char* hardware_id, char* out_name,
                                            uint32_t out_name_size) {
    if (!context || !hardware_id || !out_name || out_name_size == 0) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        std::string name;
        if (!context->profiles.lastUsed(hardware_id, name)) {
            return FGC_ERR_NOT_FOUND;
        }
        return copyName(name, out_name, out_name_size);
    });
}

FGC_API FgcResult fgc_check_controller_limit(FgcContext* context, uint32_t virtual_requested,
                                             FgcControllerLimit* out_limit) {
    if (!context || !validStruct(out_limit)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        *out_limit = limitFor(*context, virtual_requested);
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_start(FgcContext* context, const char* hardware_id) {
    if (!context || !hardware_id) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        const std::string id = hardware_id;

        const auto existing = context->sessions.find(id);
        if (existing != context->sessions.end()) {
            if (!existing->second->finished()) {
                return FGC_ERR_ALREADY_RUNNING;
            }
            context->sessions.erase(existing);
        }

        if (context->keyboard->driverState() != fgc::DriverState::Installed || !context->padBus->isAvailable()) {
            return FGC_ERR_DRIVER_MISSING;
        }
        if (!limitFor(*context, 1).allowed) {
            return FGC_ERR_CONTROLLER_LIMIT;
        }

        // Plug the pad in before capturing, so no keystroke is swallowed without a pad to receive it.
        std::unique_ptr<fgc::IVirtualPad> pad;
        FgcResult result = context->padBus->plugIn(pad);
        if (result != FGC_OK) {
            return result;
        }

        context->identify.reset();  // it holds every idle keyboard open
        std::unique_ptr<fgc::IKeyboardSession> keyboard;
        result = context->keyboard->open(id, keyboard);
        if (result != FGC_OK) {
            return result;
        }

        auto session = std::make_unique<fgc::MappingSession>(id, std::move(keyboard), std::move(pad),
                                                             currentKeys(*context, id), context->runningSessions);
        session->start();
        context->sessions.emplace(id, std::move(session));
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_stop(FgcContext* context, const char* hardware_id) {
    if (!context || !hardware_id) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        const auto session = context->sessions.find(hardware_id);
        if (session == context->sessions.end()) {
            return FGC_ERR_NOT_RUNNING;
        }
        context->sessions.erase(session);
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_stop_all(FgcContext* context) {
    if (!context) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        context->sessions.clear();
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_get_status(FgcContext* context, FgcStatus* out_status) {
    if (!context || !validStruct(out_status)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    out_status->running_sessions = context->runningSessions.load(std::memory_order_relaxed);
    return FGC_OK;
}

FGC_API FgcResult fgc_poll_event(FgcContext* context, FgcEvent* out_event) {
    if (!context || !validStruct(out_event)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    out_event->type = FGC_EVENT_NONE;
    out_event->hardware_id[0] = '\0';
    return FGC_OK;
}

FGC_API FgcResult fgc_is_running(FgcContext* context, const char* hardware_id, int32_t* out_running) {
    if (!context || !hardware_id || !out_running) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        const auto session = context->sessions.find(hardware_id);
        *out_running = session != context->sessions.end() && !session->second->finished() ? 1 : 0;
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_get_latency_stats(FgcContext* context, const char* hardware_id,
                                        FgcLatencyStats* out_stats) {
    if (!context || !hardware_id || !validStruct(out_stats)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        const auto session = context->sessions.find(hardware_id);
        if (session == context->sessions.end()) {
            return FGC_ERR_NOT_RUNNING;
        }
        const fgc::LatencySnapshot snapshot = session->second->latency();
        out_stats->sample_count = snapshot.samples;
        out_stats->last_ns = ticksToNanoseconds(snapshot.lastTicks);
        out_stats->average_ns = ticksToNanoseconds(snapshot.averageTicks);
        out_stats->max_ns = ticksToNanoseconds(snapshot.maxTicks);
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_reset_latency_stats(FgcContext* context, const char* hardware_id) {
    if (!context || !hardware_id) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        const auto session = context->sessions.find(hardware_id);
        if (session == context->sessions.end()) {
            return FGC_ERR_NOT_RUNNING;
        }
        session->second->resetLatency();
        return FGC_OK;
    });
}

}  // extern "C"
