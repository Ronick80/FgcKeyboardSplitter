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

    bool identifying_ = false;
    double identifyStartedAt_ = 0.0;

    FgcStatus status_{};
    std::string actionMessage_;
};
