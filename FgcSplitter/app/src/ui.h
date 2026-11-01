#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "fgc_core.h"

// Dear ImGui front end. Talks to the core only through fgc_core.h.
class AppUi {
public:
    explicit AppUi(FgcContext* context);

    void draw();

    // A key pressed in this window, as a key code in the FgcMapping encoding.
    void onKeyDown(uint16_t keyCode);

private:
    // Which profile a keyboard's mapping came from, and whether it was edited since.
    struct ProfileUiState {
        std::string current;
        bool dirty = false;
    };

    ProfileUiState& profileState();
    const char* selectedHardwareId() const;
    void refreshDrivers();
    void refreshKeyboards();
    void loadKeyboardList();
    void selectKeyboard(int index);
    void applyMapping();
    void startIdentify();
    void pollIdentify();
    void drawStatusBar();
    void drawDrivers();
    void drawKeyboards();
    void drawControls();
    void drawMapping();
    void drawMappingRow(FgcXboxInput input);
    void drawProfiles();
    void refreshProfiles();
    void loadProfile(const std::string& name);
    bool saveProfile(const std::string& name);
    bool driversReady() const;
    const std::string& keyName(uint16_t keyCode);

    FgcContext* context_;

    FgcDriverStatus driverStatus_{};
    FgcResult driverResult_ = FGC_OK;

    std::vector<FgcKeyboardInfo> keyboards_;
    FgcResult keyboardsResult_ = FGC_OK;
    int selectedKeyboard_ = -1;

    FgcMapping mapping_{};
    int bindingInput_ = -1;  // input waiting for a key press, or -1
    std::vector<uint16_t> pickableKeys_;
    std::map<uint16_t, std::string> keyNames_;

    std::map<std::string, ProfileUiState> profileStates_;  // by keyboard hardware ID
    std::vector<std::string> profileNames_;                // for the selected keyboard
    char nameInput_[FGC_PROFILE_NAME_MAX] = {};
    std::string profileMessage_;
    std::string popupError_;

    bool identifying_ = false;
    double identifyStartedAt_ = 0.0;

    FgcStatus status_{};
    std::string actionMessage_;
};
