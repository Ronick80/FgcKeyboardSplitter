#include "ui.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "imgui.h"

namespace {

const ImVec4 kGreen(0.35f, 0.85f, 0.45f, 1.0f);
const ImVec4 kRed(0.95f, 0.40f, 0.40f, 1.0f);
const ImVec4 kAmber(0.95f, 0.75f, 0.30f, 1.0f);
const ImVec4 kGrey(0.65f, 0.65f, 0.65f, 1.0f);

constexpr double kIdentifyTimeoutSeconds = 15.0;

const char* const kInputNames[FGC_INPUT_COUNT] = {
    "A",
    "B",
    "X",
    "Y",
    "Left bumper (LB)",
    "Right bumper (RB)",
    "Left trigger (LT)",
    "Right trigger (RT)",
    "Left stick click",
    "Right stick click",
    "Start",
    "Back",
    "Guide",
    "D-pad up",
    "D-pad down",
    "D-pad left",
    "D-pad right",
    "Left stick up",
    "Left stick down",
    "Left stick left",
    "Left stick right",
    "Right stick up",
    "Right stick down",
    "Right stick left",
    "Right stick right",
};

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
            return "No free controller slot (Windows allows 4; ScpVBus slots can also be stuck until it restarts)";
        case FGC_ERR_NOT_FOUND:
            return "Not found";
        case FGC_ERR_IO:
            return "I/O error";
        case FGC_ERR_INTERNAL:
            return "Internal error";
        case FGC_ERR_ACCESS_DENIED:
            return "Access denied (run as administrator)";
        case FGC_ERR_DEVICE_BUSY:
            return "Keyboard is in use by another program";
        case FGC_ERR_BAD_FORMAT:
            return "The file is damaged or from a newer version";
        case FGC_ERR_ALREADY_EXISTS:
            return "A profile with that name already exists";
    }
    return "Unknown error";
}

// Profile names become file names, so invalid ones are the common failure when saving or renaming.
const char* describeNameError(FgcResult result) {
    if (result == FGC_ERR_INVALID_ARGUMENT) {
        return "Use 1-63 characters without \\ / : * ? \" < > |, with no leading or trailing space or trailing dot.";
    }
    return describe(result);
}

bool equalsIgnoreCase(const std::string& a, const std::string& b) {
    return _stricmp(a.c_str(), b.c_str()) == 0;
}

// False when Windows has no name for the scan code.
bool lookupKeyName(uint16_t keyCode, std::string& name) {
    const LONG lParam =
        static_cast<LONG>(((keyCode & 0xFFu) << 16) | ((keyCode & FGC_KEY_EXTENDED_FLAG) ? (1u << 24) : 0u));
    wchar_t wide[64];
    const int length = GetKeyNameTextW(lParam, wide, 64);
    if (length <= 0) {
        return false;
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    name.resize(static_cast<size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, wide, length, name.data(), size, nullptr, nullptr);
    return true;
}

}  // namespace

AppUi::AppUi(FgcContext* context) : context_(context) {
    mapping_.struct_size = sizeof(mapping_);
    fgc_get_default_mapping(&mapping_);

    // Scan codes 0x01-0x7F, plus the E0-prefixed set; skip codes Windows has no name for.
    for (uint16_t base : {uint16_t{0}, uint16_t{FGC_KEY_EXTENDED_FLAG}}) {
        for (uint16_t scan = 1; scan < 0x80; ++scan) {
            const bool fakeShift = base != 0 && (scan == 0x2A || scan == 0x36);
            std::string name;
            if (!fakeShift && lookupKeyName(static_cast<uint16_t>(base | scan), name)) {
                pickableKeys_.push_back(static_cast<uint16_t>(base | scan));
            }
        }
    }

    refreshDrivers();
    refreshKeyboards();
}

const std::string& AppUi::keyName(uint16_t keyCode) {
    const auto cached = keyNames_.find(keyCode);
    if (cached != keyNames_.end()) {
        return cached->second;
    }

    std::string name;
    if (keyCode == FGC_KEY_UNBOUND) {
        name = "(none)";
    } else if (!lookupKeyName(keyCode, name)) {
        char fallback[16];
        snprintf(fallback, sizeof(fallback), "Key 0x%X", keyCode);
        name = fallback;
    }
    return keyNames_.emplace(keyCode, std::move(name)).first->second;
}

void AppUi::onKeyDown(uint16_t keyCode) {
    if (bindingInput_ < 0) {
        return;
    }
    const int input = bindingInput_;
    bindingInput_ = -1;
    mapping_.key_for_input[input] = keyCode;
    applyMapping();
}

void AppUi::applyMapping() {
    if (selectedKeyboard_ < 0) {
        return;
    }
    profileState().dirty = true;
    const FgcResult result = fgc_set_mapping(context_, keyboards_[selectedKeyboard_].hardware_id, &mapping_);
    if (result != FGC_OK) {
        actionMessage_ = std::string("Mapping: ") + describe(result);
    }
}

AppUi::ProfileUiState& AppUi::profileState() {
    return profileStates_[keyboards_[selectedKeyboard_].hardware_id];
}

const char* AppUi::selectedHardwareId() const {
    return keyboards_[selectedKeyboard_].hardware_id;
}

void AppUi::selectKeyboard(int index) {
    selectedKeyboard_ = index;
    bindingInput_ = -1;
    profileMessage_.clear();
    profileNames_.clear();
    if (index < 0) {
        return;
    }

    const char* hardwareId = keyboards_[index].hardware_id;
    fgc_get_mapping(context_, hardwareId, &mapping_);  // the core starts a keyboard from its last-used profile
    if (profileStates_.find(hardwareId) == profileStates_.end()) {
        ProfileUiState state;
        char lastUsed[FGC_PROFILE_NAME_MAX];
        if (fgc_profile_get_last_used(context_, hardwareId, lastUsed, sizeof(lastUsed)) == FGC_OK) {
            state.current = lastUsed;
        }
        profileStates_[hardwareId] = state;
    }
    refreshProfiles();
}

void AppUi::refreshProfiles() {
    profileNames_.clear();
    uint32_t count = 0;
    if (fgc_profile_count(context_, selectedHardwareId(), &count) != FGC_OK) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        char name[FGC_PROFILE_NAME_MAX];
        if (fgc_profile_get_name(context_, selectedHardwareId(), i, name, sizeof(name)) == FGC_OK) {
            profileNames_.push_back(name);
        }
    }
}

void AppUi::loadProfile(const std::string& name) {
    const FgcResult result = fgc_profile_load(context_, selectedHardwareId(), name.c_str(), &mapping_);
    if (result != FGC_OK) {
        profileMessage_ = "Load: " + std::string(describe(result));
        return;
    }
    profileState() = {name, false};
    profileMessage_.clear();
    bindingInput_ = -1;
}

bool AppUi::saveProfile(const std::string& name) {
    const FgcResult result = fgc_profile_save(context_, selectedHardwareId(), name.c_str(), &mapping_);
    if (result != FGC_OK) {
        popupError_ = describeNameError(result);
        profileMessage_ = "Save: " + std::string(describe(result));
        return false;
    }
    profileMessage_.clear();
    refreshProfiles();
    // Names match case-insensitively, so keep the spelling the file already has.
    std::string stored = name;
    for (const std::string& existing : profileNames_) {
        if (equalsIgnoreCase(existing, name)) {
            stored = existing;
        }
    }
    profileState() = {stored, false};
    return true;
}

void AppUi::startIdentify() {
    const FgcResult result = fgc_identify_begin(context_);
    identifying_ = result == FGC_OK;
    identifyStartedAt_ = ImGui::GetTime();
    actionMessage_ = identifying_ ? std::string() : std::string("Identify: ") + describe(result);
}

void AppUi::pollIdentify() {
    if (!identifying_) {
        return;
    }
    int32_t index = -1;
    const FgcResult result = fgc_identify_poll(context_, &index);
    const bool timedOut = ImGui::GetTime() - identifyStartedAt_ > kIdentifyTimeoutSeconds;
    if (result == FGC_OK && index < 0 && !timedOut) {
        return;
    }

    fgc_identify_cancel(context_);
    identifying_ = false;
    if (index >= 0) {
        loadKeyboardList();  // the core may have rescanned while resolving the keyboard
        if (index < static_cast<int>(keyboards_.size())) {
            selectKeyboard(index);
        }
    }
}

void AppUi::refreshDrivers() {
    driverStatus_ = {};
    driverStatus_.struct_size = sizeof(driverStatus_);
    driverResult_ = fgc_get_driver_status(context_, &driverStatus_);
}

void AppUi::refreshKeyboards() {
    selectKeyboard(-1);
    keyboardsResult_ = fgc_refresh_keyboards(context_);
    loadKeyboardList();
}

void AppUi::loadKeyboardList() {
    keyboards_.clear();
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
    updateRunning();
    if (ImGui::GetTime() - limitCheckedAt_ >= 1.0) {
        refreshLimit();
    }
    pollIdentify();

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
        ImGui::Spacing();
        drawLatency();
        ImGui::Spacing();
        drawMapping();
    }
    ImGui::End();
}

void AppUi::updateRunning() {
    runningFlags_.assign(keyboards_.size(), 0);
    for (size_t i = 0; i < keyboards_.size(); ++i) {
        int32_t running = 0;
        if (fgc_is_running(context_, keyboards_[i].hardware_id, &running) == FGC_OK) {
            runningFlags_[i] = running != 0;
        }
    }
}

bool AppUi::selectedRunning() const {
    return selectedKeyboard_ >= 0 && selectedKeyboard_ < static_cast<int>(runningFlags_.size()) &&
           runningFlags_[selectedKeyboard_] != 0;
}

void AppUi::refreshLimit() {
    limitCheckedAt_ = ImGui::GetTime();
    limit_ = {};
    limit_.struct_size = sizeof(limit_);
    if (fgc_check_controller_limit(context_, 1, &limit_) != FGC_OK) {
        limit_.allowed = 1;  // an unreadable count should not block the user; Start still enforces the limit
    }
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
    ImGui::SameLine();
    if (identifying_) {
        if (ImGui::Button("Cancel identify")) {
            fgc_identify_cancel(context_);
            identifying_ = false;
        }
    } else if (ImGui::Button("Identify by key press")) {
        startIdentify();
    }
    if (identifying_) {
        ImGui::TextColored(kAmber, "Press any key on the keyboard you want to use...");
    }

    if (keyboardsResult_ != FGC_OK) {
        ImGui::TextColored(kAmber, "Keyboard list unavailable: %s", describe(keyboardsResult_));
    } else if (keyboards_.empty()) {
        ImGui::TextColored(kGrey, "No keyboards found.");
    } else {
        for (int i = 0; i < static_cast<int>(keyboards_.size()); ++i) {
            const bool running = i < static_cast<int>(runningFlags_.size()) && runningFlags_[i] != 0;
            const std::string label =
                (running ? "[RUNNING] " : "") + std::string(keyboards_[i].display_name) + "##" + std::to_string(i);
            if (running) {
                ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
            }
            if (ImGui::Selectable(label.c_str(), selectedKeyboard_ == i)) {
                selectKeyboard(i);
            }
            if (running) {
                ImGui::PopStyleColor();
            }
        }
    }
}

void AppUi::drawControls() {
    ImGui::SeparatorText("Control");

    const bool selectedIsRunning = selectedRunning();
    const bool canStart = !selectedIsRunning && selectedKeyboard_ >= 0 && driversReady() && limit_.allowed != 0;

    ImGui::BeginDisabled(!canStart);
    if (ImGui::Button("Start", ImVec2(120, 0))) {
        const FgcResult result = fgc_start(context_, selectedHardwareId());
        actionMessage_ = std::string("Start: ") + describe(result);
        limitCheckedAt_ = -10.0;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!selectedIsRunning);
    if (ImGui::Button("Stop", ImVec2(120, 0))) {
        const FgcResult result = fgc_stop(context_, selectedHardwareId());
        actionMessage_ = std::string("Stop: ") + describe(result);
        limitCheckedAt_ = -10.0;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(status_.running_sessions == 0);
    if (ImGui::Button("Stop all", ImVec2(120, 0))) {
        const FgcResult result = fgc_stop_all(context_);
        actionMessage_ = std::string("Stop all: ") + describe(result);
        limitCheckedAt_ = -10.0;
    }
    ImGui::EndDisabled();

    ImGui::Text("Controller slots: %u other + %u from this app = %u of %u", limit_.physical_connected,
                limit_.virtual_active, limit_.physical_connected + limit_.virtual_active,
                FGC_MAX_XINPUT_CONTROLLERS);
    if (!limit_.allowed && !selectedIsRunning) {
        ImGui::TextColored(kRed, "No free slot: Windows supports at most 4 controllers in total.");
    }

    ImGui::TextWrapped(
        "While running, every key on a started keyboard is hidden from other programs. If it is your only "
        "keyboard, click Stop with the mouse.");

    if (!actionMessage_.empty()) {
        ImGui::TextUnformatted(actionMessage_.c_str());
    }
}

void AppUi::drawLatency() {
    if (status_.running_sessions == 0) {
        return;
    }

    ImGui::SeparatorText("Latency");
    ImGui::TextWrapped(
        "Microseconds from the mapping thread waking on a key event to the virtual controller accepting the "
        "report. It does not include keyboard or USB delay, or how often the game reads the controller.");

    if (ImGui::BeginTable("latency", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("Samples");
        ImGui::TableSetupColumn("Last");
        ImGui::TableSetupColumn("Average");
        ImGui::TableSetupColumn("Max");
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < keyboards_.size(); ++i) {
            FgcLatencyStats stats{};
            stats.struct_size = sizeof(stats);
            if (i >= runningFlags_.size() || runningFlags_[i] == 0 ||
                fgc_get_latency_stats(context_, keyboards_[i].hardware_id, &stats) != FGC_OK) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(keyboards_[i].display_name);
            ImGui::TableNextColumn();
            ImGui::Text("%llu", static_cast<unsigned long long>(stats.sample_count));
            for (const uint64_t ns : {stats.last_ns, stats.average_ns, stats.max_ns}) {
                ImGui::TableNextColumn();
                if (stats.sample_count == 0) {
                    ImGui::TextUnformatted("-");
                } else {
                    ImGui::Text("%.1f", static_cast<double>(ns) / 1000.0);
                }
            }
        }
        ImGui::EndTable();
    }

    if (ImGui::Button("Reset statistics")) {
        for (size_t i = 0; i < keyboards_.size(); ++i) {
            if (i < runningFlags_.size() && runningFlags_[i] != 0) {
                fgc_reset_latency_stats(context_, keyboards_[i].hardware_id);
            }
        }
    }
}

void AppUi::drawMapping() {
    if (selectedKeyboard_ < 0) {
        ImGui::SeparatorText("Mapping");
        ImGui::TextColored(kGrey, "Select a keyboard to edit its mapping.");
        return;
    }

    drawProfiles();
    ImGui::Spacing();
    ImGui::SeparatorText("Mapping");
    ImGui::TextWrapped(
        "Click a key button, then press the new key; click the button again to cancel. Use \"...\" to pick "
        "from a list instead; that also works while running, when the selected keyboard is hidden from this "
        "window. Changes apply immediately.");

    if (ImGui::Button("Reset to defaults")) {
        fgc_get_default_mapping(&mapping_);
        applyMapping();
    }
    ImGui::Spacing();

    const auto drawGroup = [this](const char* title, std::initializer_list<FgcXboxInput> inputs) {
        ImGui::SeparatorText(title);
        for (const FgcXboxInput input : inputs) {
            drawMappingRow(input);
        }
    };

    if (ImGui::BeginTable("mapping", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextColumn();
        drawGroup("Face buttons", {FGC_INPUT_A, FGC_INPUT_B, FGC_INPUT_X, FGC_INPUT_Y});
        drawGroup("Shoulders and triggers", {FGC_INPUT_LEFT_BUMPER, FGC_INPUT_RIGHT_BUMPER, FGC_INPUT_LEFT_TRIGGER,
                                             FGC_INPUT_RIGHT_TRIGGER});
        drawGroup("Menu", {FGC_INPUT_START, FGC_INPUT_BACK, FGC_INPUT_GUIDE});
        drawGroup("Stick clicks", {FGC_INPUT_LEFT_STICK_CLICK, FGC_INPUT_RIGHT_STICK_CLICK});

        ImGui::TableNextColumn();
        drawGroup("D-pad", {FGC_INPUT_DPAD_UP, FGC_INPUT_DPAD_DOWN, FGC_INPUT_DPAD_LEFT, FGC_INPUT_DPAD_RIGHT});
        drawGroup("Left stick", {FGC_INPUT_LEFT_STICK_UP, FGC_INPUT_LEFT_STICK_DOWN, FGC_INPUT_LEFT_STICK_LEFT,
                                 FGC_INPUT_LEFT_STICK_RIGHT});
        drawGroup("Right stick", {FGC_INPUT_RIGHT_STICK_UP, FGC_INPUT_RIGHT_STICK_DOWN, FGC_INPUT_RIGHT_STICK_LEFT,
                                  FGC_INPUT_RIGHT_STICK_RIGHT});
        ImGui::EndTable();
    }
}

void AppUi::drawMappingRow(FgcXboxInput input) {
    const float font = ImGui::GetFontSize();
    const uint16_t key = mapping_.key_for_input[input];
    const bool binding = bindingInput_ == static_cast<int>(input);

    bool shared = false;
    for (int other = 0; other < FGC_INPUT_COUNT; ++other) {
        shared = shared || (other != static_cast<int>(input) && key != FGC_KEY_UNBOUND &&
                            mapping_.key_for_input[other] == key);
    }

    ImGui::PushID(static_cast<int>(input));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(kInputNames[input]);
    ImGui::SameLine(font * 10.0f);

    if (shared) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAmber);
    }
    const std::string label = binding ? std::string("Press a key...") : keyName(key);
    if (ImGui::Button(label.c_str(), ImVec2(font * 8.0f, 0))) {
        bindingInput_ = binding ? -1 : static_cast<int>(input);
    }
    if (shared) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("This key is also bound to another input.");
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("...")) {
        ImGui::OpenPopup("pick");
    }
    if (ImGui::BeginPopup("pick")) {
        if (ImGui::Selectable("(none)", key == FGC_KEY_UNBOUND)) {
            mapping_.key_for_input[input] = FGC_KEY_UNBOUND;
            applyMapping();
        }
        ImGui::Separator();
        ImGui::BeginChild("keys", ImVec2(font * 14.0f, font * 16.0f));
        for (const uint16_t code : pickableKeys_) {
            ImGui::PushID(code);
            if (ImGui::Selectable(keyName(code).c_str(), key == code)) {
                mapping_.key_for_input[input] = code;
                applyMapping();
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void AppUi::drawProfiles() {
    const float font = ImGui::GetFontSize();
    ProfileUiState& state = profileState();

    ImGui::SeparatorText("Profile");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Current:");
    ImGui::SameLine();
    if (state.current.empty()) {
        ImGui::TextColored(kGrey, "(no profile)");
    } else {
        ImGui::TextUnformatted(state.current.c_str());
    }
    if (state.dirty) {
        ImGui::SameLine();
        ImGui::TextColored(kAmber, "- unsaved changes");
    }

    ImGui::SetNextItemWidth(font * 14.0f);
    if (ImGui::BeginCombo("##load", "Load a profile...")) {
        for (const std::string& name : profileNames_) {
            if (ImGui::Selectable(name.c_str(), name == state.current)) {
                loadProfile(name);
            }
        }
        if (profileNames_.empty()) {
            ImGui::TextDisabled("No saved profiles");
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(state.current.empty() || !state.dirty);
    if (ImGui::Button("Save")) {
        saveProfile(state.current);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Save as...")) {
        snprintf(nameInput_, sizeof(nameInput_), "%s", state.current.c_str());
        popupError_.clear();
        bindingInput_ = -1;
        ImGui::OpenPopup("Save profile as");
    }

    ImGui::BeginDisabled(state.current.empty());
    ImGui::SameLine();
    if (ImGui::Button("Rename...")) {
        snprintf(nameInput_, sizeof(nameInput_), "%s", state.current.c_str());
        popupError_.clear();
        bindingInput_ = -1;
        ImGui::OpenPopup("Rename profile");
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete...")) {
        popupError_.clear();
        bindingInput_ = -1;
        ImGui::OpenPopup("Delete profile");
    }
    ImGui::EndDisabled();

    if (ImGui::BeginPopupModal("Save profile as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::TextUnformatted("Profile name:");
        const bool enter = ImGui::InputText("##name", nameInput_, sizeof(nameInput_), ImGuiInputTextFlags_EnterReturnsTrue);
        bool replaces = false;
        for (const std::string& existing : profileNames_) {
            replaces = replaces || equalsIgnoreCase(existing, nameInput_);
        }
        if (replaces) {
            ImGui::TextColored(kAmber, "A profile with this name exists and will be replaced.");
        }
        if (!popupError_.empty()) {
            ImGui::TextColored(kRed, "%s", popupError_.c_str());
        }
        if ((ImGui::Button("Save", ImVec2(font * 6.0f, 0)) || enter) && saveProfile(nameInput_)) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(font * 6.0f, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Rename profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::TextUnformatted("New name:");
        const bool enter = ImGui::InputText("##name", nameInput_, sizeof(nameInput_), ImGuiInputTextFlags_EnterReturnsTrue);
        if (!popupError_.empty()) {
            ImGui::TextColored(kRed, "%s", popupError_.c_str());
        }
        if (ImGui::Button("Rename", ImVec2(font * 6.0f, 0)) || enter) {
            const FgcResult result = fgc_profile_rename(context_, selectedHardwareId(), state.current.c_str(), nameInput_);
            if (result == FGC_OK) {
                state.current = nameInput_;
                refreshProfiles();
                ImGui::CloseCurrentPopup();
            } else {
                popupError_ = describeNameError(result);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(font * 6.0f, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopupModal("Delete profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Delete the profile \"%s\"?", state.current.c_str());
        ImGui::TextUnformatted("The keyboard keeps its current mapping, but it will no longer be saved.");
        if (!popupError_.empty()) {
            ImGui::TextColored(kRed, "%s", popupError_.c_str());
        }
        if (ImGui::Button("Delete", ImVec2(font * 6.0f, 0))) {
            const FgcResult result = fgc_profile_delete(context_, selectedHardwareId(), state.current.c_str());
            if (result == FGC_OK) {
                state = {};
                refreshProfiles();
                ImGui::CloseCurrentPopup();
            } else {
                popupError_ = describe(result);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(font * 6.0f, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!profileMessage_.empty()) {
        ImGui::TextColored(kRed, "%s", profileMessage_.c_str());
    }
}
