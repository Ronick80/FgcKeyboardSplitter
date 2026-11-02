#pragma once

#include "scpvbus_installer.h"

namespace setup {

// Fetches the official Interception release (checked against a pinned SHA-256) and runs its installer.
Outcome installInterception(const Env& env);
Outcome uninstallInterception(const Env& env);

}  // namespace setup
