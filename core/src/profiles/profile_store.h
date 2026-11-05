#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "engine/mapping_table.h"

namespace fgc {

// Stores named mappings per keyboard. The file layout is documented in fgc_core.h.
// Not thread-safe; the context's control lock serializes callers.
class ProfileStore {
public:
    explicit ProfileStore(std::filesystem::path root);

    // %APPDATA%\FgcKeyboardSplitter
    static std::filesystem::path defaultRoot();

    // Built-in profiles first, in their fixed order, then the keyboard's own sorted case-insensitively.
    FgcResult list(const std::string& hardwareId, std::vector<std::string>& names) const;

    // True for a built-in profile name (case-insensitive). Those cannot be saved over, renamed or deleted.
    static bool isBuiltin(const std::string& name);
    FgcResult load(const std::string& hardwareId, const std::string& name, KeyArray& keys) const;
    FgcResult save(const std::string& hardwareId, const std::string& name, const KeyArray& keys) const;
    FgcResult rename(const std::string& hardwareId, const std::string& from, const std::string& to) const;
    FgcResult remove(const std::string& hardwareId, const std::string& name) const;

    // The "Default" built-in profile when none is recorded or its file is gone. False only for a hardware ID
    // that cannot name a profile folder.
    bool lastUsed(const std::string& hardwareId, std::string& name) const;
    void setLastUsed(const std::string& hardwareId, const std::string& name) const;

private:
    // False when none is recorded or its file is gone.
    bool storedLastUsed(const std::string& hardwareId, std::string& name) const;
    void clearLastUsed(const std::string& hardwareId) const;

    std::filesystem::path root_;
};

}  // namespace fgc
