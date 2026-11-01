#pragma once

#include <string>
#include <vector>

#include "fgc_core.h"

// Dear ImGui front end. Talks to the core only through fgc_core.h.
class AppUi {
public:
    explicit AppUi(FgcContext* context);

    void draw();

private:
    void refreshDrivers();
    void refreshKeyboards();
    void drawStatusBar();
    void drawDrivers();
    void drawKeyboards();
    void drawControls();
    bool driversReady() const;

    FgcContext* context_;

    FgcDriverStatus driverStatus_{};
    FgcResult driverResult_ = FGC_OK;

    std::vector<FgcKeyboardInfo> keyboards_;
    FgcResult keyboardsResult_ = FGC_OK;
    int selectedKeyboard_ = -1;

    FgcStatus status_{};
    std::string actionMessage_;
};
