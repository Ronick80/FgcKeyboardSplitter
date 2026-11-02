#pragma once

#include <string>

#include "fgc_core.h"

namespace setup {

enum class Outcome {
    Done,
    AlreadyDone,
    RestartRequired,
    Failed,
    Unsupported,  // not available in this build
};

struct Env {
    FgcContext* core = nullptr;
    bool dryRun = false;  // report what would happen, change nothing
};

// infPath is the absolute path of ScpVBus.inf.
Outcome installScpVBus(const Env& env, const std::wstring& infPath);
Outcome uninstallScpVBus(const Env& env);

// A readable explanation of a Windows or SetupAPI error code, with advice for the common signature failures.
std::wstring describeSetupError(unsigned long error);

}  // namespace setup
