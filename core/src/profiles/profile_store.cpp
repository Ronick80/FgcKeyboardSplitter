#include "profile_store.h"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <system_error>

namespace fgc {
namespace {

namespace fs = std::filesystem;
using json = nlohmann::json;

constexpr const char* kProfileFormat = "fgc-profile";
constexpr const char* kLastUsedFormat = "fgc-last-used";
constexpr int kFormatVersion = 1;
constexpr size_t kMaxNameBytes = FGC_PROFILE_NAME_MAX - 1;
constexpr const wchar_t* kInvalidPathChars = L"<>:\"/\\|?*";

// Order matches FgcXboxInput.
constexpr const char* kBindingNames[FGC_INPUT_COUNT] = {
    "a",
    "b",
    "x",
    "y",
    "left_bumper",
    "right_bumper",
    "left_trigger",
    "right_trigger",
    "left_stick_click",
    "right_stick_click",
    "start",
    "back",
    "guide",
    "dpad_up",
    "dpad_down",
    "dpad_left",
    "dpad_right",
    "left_stick_up",
    "left_stick_down",
    "left_stick_left",
    "left_stick_right",
    "right_stick_up",
    "right_stick_down",
    "right_stick_left",
    "right_stick_right",
};

bool toWide(const std::string& utf8, std::wstring& wide) {
    wide.clear();
    if (utf8.empty()) {
        return true;
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return false;
    }
    wide.resize(static_cast<size_t>(size));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return true;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring upper(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towupper(c)); });
    return text;
}

bool sameName(const std::string& a, const std::string& b) {
    std::wstring wideA;
    std::wstring wideB;
    return toWide(a, wideA) && toWide(b, wideB) && upper(wideA) == upper(wideB);
}

bool isReservedDeviceName(const std::wstring& name) {
    const std::wstring stem = upper(name.substr(0, name.find(L'.')));
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") {
        return true;
    }
    return stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) && stem[3] >= L'1' &&
           stem[3] <= L'9';
}

// The profile name becomes the file name, so anything Windows would reject or alias is refused up front.
bool validateName(const std::string& name, std::wstring& wide) {
    if (name.empty() || name.size() > kMaxNameBytes || !toWide(name, wide)) {
        return false;
    }
    for (const wchar_t c : wide) {
        if (c < 0x20 || wcschr(kInvalidPathChars, c) != nullptr) {
            return false;
        }
    }
    return wide.front() != L' ' && wide.back() != L' ' && wide.back() != L'.' && !isReservedDeviceName(wide);
}

bool writeFileAtomically(const fs::path& path, const std::string& content) {
    fs::path temporary = path;
    temporary += L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out) {
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        return false;
    }
    return true;
}

FgcResult readJson(const fs::path& path, json& document) {
    std::error_code error;
    if (!fs::is_regular_file(path, error)) {
        return FGC_ERR_NOT_FOUND;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return FGC_ERR_IO;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    document = json::parse(text, nullptr, false);
    return document.is_discarded() ? FGC_ERR_BAD_FORMAT : FGC_OK;
}

std::string dump(const json& document) {
    return document.dump(2, ' ', false, json::error_handler_t::replace);
}

bool keyboardDirectoryName(const std::string& hardwareId, std::wstring& name) {
    if (hardwareId.empty() || !toWide(hardwareId, name)) {
        return false;
    }
    for (wchar_t& c : name) {
        if (c < 0x20 || wcschr(kInvalidPathChars, c) != nullptr) {
            c = L'_';
        }
    }
    while (!name.empty() && (name.back() == L'.' || name.back() == L' ')) {
        name.back() = L'_';
    }
    return true;
}

}  // namespace

ProfileStore::ProfileStore(fs::path root) : root_(std::move(root)) {}

fs::path ProfileStore::defaultRoot() {
    PWSTR appData = nullptr;
    fs::path root;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        root = fs::path(appData) / L"FgcKeyboardSplitter";
    } else {
        root = fs::temp_directory_path() / L"FgcKeyboardSplitter";
    }
    CoTaskMemFree(appData);
    return root;
}

FgcResult ProfileStore::list(const std::string& hardwareId, std::vector<std::string>& names) const {
    names.clear();
    std::wstring directoryName;
    if (!keyboardDirectoryName(hardwareId, directoryName)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }

    std::error_code error;
    fs::directory_iterator entry(root_ / L"profiles" / directoryName, error);
    if (error) {
        return FGC_OK;  // no folder yet means no profiles
    }
    for (const fs::directory_iterator end; entry != end; entry.increment(error)) {
        if (error) {
            return FGC_ERR_IO;
        }
        const fs::path& path = entry->path();
        if (entry->is_regular_file(error) && upper(path.extension().wstring()) == L".JSON") {
            names.push_back(toUtf8(path.stem().wstring()));
        }
    }

    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        std::wstring wideA;
        std::wstring wideB;
        toWide(a, wideA);
        toWide(b, wideB);
        return upper(wideA) < upper(wideB);
    });
    return FGC_OK;
}

FgcResult ProfileStore::load(const std::string& hardwareId, const std::string& name, KeyArray& keys) const {
    std::wstring wideName;
    std::wstring directoryName;
    if (!validateName(name, wideName) || !keyboardDirectoryName(hardwareId, directoryName)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }

    json document;
    const FgcResult read = readJson(root_ / L"profiles" / directoryName / (wideName + L".json"), document);
    if (read != FGC_OK) {
        return read;
    }

    if (!document.is_object() || document.value("format", "") != kProfileFormat ||
        !document.contains("version") || !document["version"].is_number_integer() ||
        document["version"].get<int>() > kFormatVersion || !document.contains("bindings") ||
        !document["bindings"].is_object()) {
        return FGC_ERR_BAD_FORMAT;
    }

    KeyArray loaded{};
    const json& bindings = document["bindings"];
    for (size_t input = 0; input < loaded.size(); ++input) {
        const auto binding = bindings.find(kBindingNames[input]);
        if (binding == bindings.end()) {
            continue;
        }
        if (!binding->is_number_unsigned() || binding->get<uint64_t>() >= kKeyCodeSpace) {
            return FGC_ERR_BAD_FORMAT;
        }
        loaded[input] = binding->get<uint16_t>();
    }
    keys = loaded;
    return FGC_OK;
}

FgcResult ProfileStore::save(const std::string& hardwareId, const std::string& name, const KeyArray& keys) const {
    std::wstring wideName;
    std::wstring directoryName;
    if (!validateName(name, wideName) || !keyboardDirectoryName(hardwareId, directoryName)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }

    json bindings = json::object();
    for (size_t input = 0; input < keys.size(); ++input) {
        bindings[kBindingNames[input]] = keys[input];
    }
    const json document = {{"format", kProfileFormat},
                           {"version", kFormatVersion},
                           {"keyboard", hardwareId},
                           {"bindings", bindings}};

    const fs::path directory = root_ / L"profiles" / directoryName;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error || !writeFileAtomically(directory / (wideName + L".json"), dump(document))) {
        return FGC_ERR_IO;
    }
    setLastUsed(hardwareId, name);
    return FGC_OK;
}

FgcResult ProfileStore::rename(const std::string& hardwareId, const std::string& from, const std::string& to) const {
    std::wstring wideFrom;
    std::wstring wideTo;
    std::wstring directoryName;
    if (!validateName(from, wideFrom) || !validateName(to, wideTo) || !keyboardDirectoryName(hardwareId, directoryName)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }

    const fs::path directory = root_ / L"profiles" / directoryName;
    const fs::path source = directory / (wideFrom + L".json");
    const fs::path target = directory / (wideTo + L".json");
    std::error_code error;
    if (!fs::is_regular_file(source, error)) {
        return FGC_ERR_NOT_FOUND;
    }
    // Names that differ only by case are the same file, so that rename is allowed.
    if (upper(wideFrom) != upper(wideTo) && fs::exists(target, error)) {
        return FGC_ERR_ALREADY_EXISTS;
    }
    // lastUsed() checks the file exists, so ask before the file moves.
    std::string last;
    const bool wasLastUsed = lastUsed(hardwareId, last) && sameName(last, from);

    fs::rename(source, target, error);
    if (error) {
        return FGC_ERR_IO;
    }
    if (wasLastUsed) {
        setLastUsed(hardwareId, to);
    }
    return FGC_OK;
}

FgcResult ProfileStore::remove(const std::string& hardwareId, const std::string& name) const {
    std::wstring wideName;
    std::wstring directoryName;
    if (!validateName(name, wideName) || !keyboardDirectoryName(hardwareId, directoryName)) {
        return FGC_ERR_INVALID_ARGUMENT;
    }

    // lastUsed() checks the file exists, so ask before it is deleted.
    std::string last;
    const bool wasLastUsed = lastUsed(hardwareId, last) && sameName(last, name);

    std::error_code error;
    const bool removed = fs::remove(root_ / L"profiles" / directoryName / (wideName + L".json"), error);
    if (error) {
        return FGC_ERR_IO;
    }
    if (!removed) {
        return FGC_ERR_NOT_FOUND;
    }
    if (wasLastUsed) {
        clearLastUsed(hardwareId);
    }
    return FGC_OK;
}

bool ProfileStore::lastUsed(const std::string& hardwareId, std::string& name) const {
    json document;
    if (readJson(root_ / L"last_used.json", document) != FGC_OK || !document.is_object() ||
        !document.contains("profiles") || !document["profiles"].is_object()) {
        return false;
    }
    const auto entry = document["profiles"].find(hardwareId);
    if (entry == document["profiles"].end() || !entry->is_string()) {
        return false;
    }

    std::wstring wideName;
    std::wstring directoryName;
    const std::string candidate = entry->get<std::string>();
    if (!validateName(candidate, wideName) || !keyboardDirectoryName(hardwareId, directoryName)) {
        return false;
    }
    std::error_code error;
    if (!fs::is_regular_file(root_ / L"profiles" / directoryName / (wideName + L".json"), error)) {
        return false;
    }
    name = candidate;
    return true;
}

void ProfileStore::setLastUsed(const std::string& hardwareId, const std::string& name) const {
    json document;
    if (readJson(root_ / L"last_used.json", document) != FGC_OK || !document.is_object() ||
        !document.contains("profiles") || !document["profiles"].is_object()) {
        document = {{"format", kLastUsedFormat}, {"version", kFormatVersion}, {"profiles", json::object()}};
    }
    document["profiles"][hardwareId] = name;

    std::error_code error;
    fs::create_directories(root_, error);
    writeFileAtomically(root_ / L"last_used.json", dump(document));
}

void ProfileStore::clearLastUsed(const std::string& hardwareId) const {
    json document;
    if (readJson(root_ / L"last_used.json", document) != FGC_OK || !document.is_object() ||
        !document.contains("profiles") || !document["profiles"].is_object()) {
        return;
    }
    document["profiles"].erase(hardwareId);
    writeFileAtomically(root_ / L"last_used.json", dump(document));
}

}  // namespace fgc
