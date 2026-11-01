#include "ui.h"

#include "imgui.h"

namespace {

const ImVec4 kGreen(0.35f, 0.85f, 0.45f, 1.0f);
const ImVec4 kRed(0.95f, 0.40f, 0.40f, 1.0f);
const ImVec4 kAmber(0.95f, 0.75f, 0.30f, 1.0f);
const ImVec4 kGrey(0.65f, 0.65f, 0.65f, 1.0f);

const char* describe(FgcResult result) {
    switch (result) {
        case FGC_OK:
            return "OK";
        case FGC_ERR_INVALID_ARGUMENT:
            return "Invalid argument";
        case FGC_ERR_NOT_IMPLEMENTED:
            return "Not implemented yet";
        case FGC_ERR_BUFFER_TOO_SMALL:
            return "Buffer too small";
        case FGC_ERR_DRIVER_MISSING:
            return "A required driver is missing";
        case FGC_ERR_ALREADY_RUNNING:
            return "Already running";
        case FGC_ERR_NOT_RUNNING:
            return "Not running";
        case FGC_ERR_CONTROLLER_LIMIT:
            return "Would exceed the 4 controller limit";
        case FGC_ERR_NOT_FOUND:
            return "Not found";
        case FGC_ERR_IO:
            return "I/O error";
        case FGC_ERR_INTERNAL:
            return "Internal error";
    }
    return "Unknown error";
}

}  // namespace

AppUi::AppUi(FgcContext* context) : context_(context) {
    refreshDrivers();
    refreshKeyboards();
}

void AppUi::refreshDrivers() {
    driverStatus_ = {};
    driverStatus_.struct_size = sizeof(driverStatus_);
    driverResult_ = fgc_get_driver_status(context_, &driverStatus_);
}

void AppUi::refreshKeyboards() {
    keyboards_.clear();
    selectedKeyboard_ = -1;

    keyboardsResult_ = fgc_refresh_keyboards(context_);
    if (keyboardsResult_ != FGC_OK) {
        return;
    }

    uint32_t count = 0;
    fgc_get_keyboard_count(context_, &count);
    for (uint32_t i = 0; i < count; ++i) {
        FgcKeyboardInfo info{};
        info.struct_size = sizeof(info);
        if (fgc_get_keyboard(context_, i, &info) == FGC_OK) {
            keyboards_.push_back(info);
        }
    }
}

bool AppUi::driversReady() const {
    return driverResult_ == FGC_OK && driverStatus_.interception == FGC_INTERCEPTION_INSTALLED &&
           driverStatus_.scpvbus_installed != 0;
}

void AppUi::draw() {
    status_ = {};
    status_.struct_size = sizeof(status_);
    fgc_get_status(context_, &status_);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("FGC Keyboard Splitter", nullptr, flags)) {
        drawStatusBar();
        ImGui::Separator();
        drawDrivers();
        ImGui::Spacing();
        drawKeyboards();
        ImGui::Spacing();
        drawControls();
    }
    ImGui::End();
}

void AppUi::drawStatusBar() {
    ImGui::TextUnformatted("Mapping:");
    ImGui::SameLine();
    if (status_.running_sessions > 0) {
        ImGui::TextColored(kGreen, "RUNNING (%u keyboard%s)", status_.running_sessions,
                           status_.running_sessions == 1 ? "" : "s");
    } else {
        ImGui::TextColored(kGrey, "STOPPED");
    }
}

void AppUi::drawDrivers() {
    ImGui::SeparatorText("Drivers");

    if (driverResult_ != FGC_OK) {
        ImGui::TextColored(kRed, "Could not query drivers: %s", describe(driverResult_));
    } else {
        ImGui::TextUnformatted("Interception (keyboard capture):");
        ImGui::SameLine();
        switch (driverStatus_.interception) {
            case FGC_INTERCEPTION_INSTALLED:
                ImGui::TextColored(kGreen, "Installed");
                break;
            case FGC_INTERCEPTION_REBOOT_REQUIRED:
                ImGui::TextColored(kAmber, "Reboot required");
                ImGui::TextWrapped("Interception is installed but not active yet. Restart Windows to finish.");
                break;
            default:
                ImGui::TextColored(kRed, "Not installed");
                ImGui::TextWrapped(
                    "Run \"install-interception.exe /install\" from an administrator prompt, then restart Windows.");
                break;
        }

        ImGui::TextUnformatted("ScpVBus (virtual Xbox controller):");
        ImGui::SameLine();
        if (driverStatus_.scpvbus_installed) {
            ImGui::TextColored(kGreen, "Installed");
        } else {
            ImGui::TextColored(kRed, "Not installed");
            ImGui::TextWrapped(
                "From an administrator prompt, run \"devcon install ScpVBus.inf Root\\ScpVBus\" in the folder "
                "containing the ScpVBus driver files.");
        }
    }

    if (ImGui::Button("Re-check drivers")) {
        refreshDrivers();
    }
}

void AppUi::drawKeyboards() {
    ImGui::SeparatorText("Keyboard");

    if (ImGui::Button("Refresh list")) {
        refreshKeyboards();
    }

    if (keyboardsResult_ != FGC_OK) {
        ImGui::TextColored(kAmber, "Keyboard list unavailable: %s", describe(keyboardsResult_));
        return;
    }
    if (keyboards_.empty()) {
        ImGui::TextColored(kGrey, "No keyboards found.");
        return;
    }

    for (int i = 0; i < static_cast<int>(keyboards_.size()); ++i) {
        if (ImGui::Selectable(keyboards_[i].display_name, selectedKeyboard_ == i)) {
            selectedKeyboard_ = i;
        }
    }
}

void AppUi::drawControls() {
    ImGui::SeparatorText("Control");

    const bool running = status_.running_sessions > 0;
    const bool canStart = !running && selectedKeyboard_ >= 0 && driversReady();

    ImGui::BeginDisabled(!canStart);
    if (ImGui::Button("Start", ImVec2(120, 0))) {
        const FgcResult result = fgc_start(context_, keyboards_[selectedKeyboard_].hardware_id);
        actionMessage_ = std::string("Start: ") + describe(result);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (ImGui::Button("Stop", ImVec2(120, 0))) {
        const FgcResult result = fgc_stop_all(context_);
        actionMessage_ = std::string("Stop: ") + describe(result);
    }
    ImGui::EndDisabled();

    if (!actionMessage_.empty()) {
        ImGui::TextUnformatted(actionMessage_.c_str());
    }
}
