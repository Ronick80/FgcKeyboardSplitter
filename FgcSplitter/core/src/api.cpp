#include "fgc_core.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "backends/interception_keyboard.h"
#include "backends/scpvbus_pad.h"
#include "context.h"

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

// Caller holds the control lock.
fgc::KeyArray keysFor(const FgcContext& context, const std::string& hardwareId) {
    const auto stored = context.mappings.find(hardwareId);
    return stored != context.mappings.end() ? stored->second : fgc::defaultKeys();
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
        const fgc::KeyArray keys = keysFor(*context, hardware_id);
        std::copy(keys.begin(), keys.end(), out_mapping->key_for_input);
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_set_mapping(FgcContext* context, const char* hardware_id, const FgcMapping* mapping) {
    if (!context || !hardware_id || !validStruct(mapping)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    fgc::KeyArray keys;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (mapping->key_for_input[i] >= fgc::kKeyCodeSpace) {
            return FGC_ERR_INVALID_ARGUMENT;
        }
        keys[i] = mapping->key_for_input[i];
    }
    return guarded([&] {
        std::lock_guard<std::mutex> lock(context->controlLock);
        context->mappings[hardware_id] = keys;
        const auto session = context->sessions.find(hardware_id);
        if (session != context->sessions.end() && !session->second->finished()) {
            session->second->setMapping(keys);
        }
        return FGC_OK;
    });
}

FGC_API FgcResult fgc_profile_count(FgcContext* context, const char* hardware_id, uint32_t* out_count) {
    if (!context || !hardware_id || !out_count) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return FGC_ERR_NOT_IMPLEMENTED;
}

FGC_API FgcResult fgc_profile_get_name(FgcContext* context, const char* hardware_id, uint32_t,
                                       char* out_name, uint32_t out_name_size) {
    if (!context || !hardware_id || !out_name || out_name_size == 0) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return FGC_ERR_NOT_IMPLEMENTED;
}

FGC_API FgcResult fgc_profile_load(FgcContext* context, const char* hardware_id, const char* name,
                                   FgcMapping* out_mapping) {
    if (!context || !hardware_id || !name || !validStruct(out_mapping)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return FGC_ERR_NOT_IMPLEMENTED;
}

FGC_API FgcResult fgc_profile_save(FgcContext* context, const char* hardware_id, const char* name,
                                   const FgcMapping* mapping) {
    if (!context || !hardware_id || !name || !validStruct(mapping)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return FGC_ERR_NOT_IMPLEMENTED;
}

FGC_API FgcResult fgc_profile_rename(FgcContext* context, const char* hardware_id, const char* old_name,
                                     const char* new_name) {
    if (!context || !hardware_id || !old_name || !new_name) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return FGC_ERR_NOT_IMPLEMENTED;
}

FGC_API FgcResult fgc_profile_delete(FgcContext* context, const char* hardware_id, const char* name) {
    if (!context || !hardware_id || !name) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return FGC_ERR_NOT_IMPLEMENTED;
}

FGC_API FgcResult fgc_profile_get_last_used(FgcContext* context, const char* hardware_id, char* out_name,
                                            uint32_t out_name_size) {
    if (!context || !hardware_id || !out_name || out_name_size == 0) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return FGC_ERR_NOT_IMPLEMENTED;
}

FGC_API FgcResult fgc_check_controller_limit(FgcContext* context, uint32_t, FgcControllerLimit* out_limit) {
    if (!context || !validStruct(out_limit)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return FGC_ERR_NOT_IMPLEMENTED;
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
                                                             keysFor(*context, id), context->runningSessions);
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

FGC_API FgcResult fgc_get_latency_stats(FgcContext* context, const char* hardware_id,
                                        FgcLatencyStats* out_stats) {
    if (!context || !hardware_id || !validStruct(out_stats)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }
    return FGC_ERR_NOT_IMPLEMENTED;
}

}  // extern "C"
