#pragma once

namespace setup {

// Writes the Windows version and the Secure Boot / memory integrity state to the log only.
void logSystemInfo();

// Console advice after a failure; installing adds hints about Windows security features that block drivers.
void printFailureHelp(bool installing);

// Offers to restart Windows (default No). With prompt false it only reminds the user.
void offerRestart(bool prompt);

}  // namespace setup
