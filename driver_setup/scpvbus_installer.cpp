#include "scpvbus_installer.h"

#include <windows.h>

#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#include <algorithm>
#include <cwchar>
#include <string>
#include <vector>

#include "log.h"

namespace setup {
namespace {

constexpr wchar_t kHardwareId[] = L"Root\\ScpVBus\0";  // multi-string: the second terminator is the implicit one

class DeviceSet {
public:
    explicit DeviceSet(HDEVINFO handle = INVALID_HANDLE_VALUE) : handle_(handle) {}
    DeviceSet(DeviceSet&& other) noexcept : handle_(other.handle_) { other.handle_ = INVALID_HANDLE_VALUE; }
    DeviceSet& operator=(DeviceSet&& other) noexcept {
        reset();
        handle_ = other.handle_;
        other.handle_ = INVALID_HANDLE_VALUE;
        return *this;
    }
    DeviceSet(const DeviceSet&) = delete;
    DeviceSet& operator=(const DeviceSet&) = delete;
    ~DeviceSet() { reset(); }

    HDEVINFO get() const { return handle_; }
    bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }

private:
    void reset() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }
    HDEVINFO handle_;
};

struct Nodes {
    DeviceSet set;
    std::vector<SP_DEVINFO_DATA> devices;
};

bool hasHardwareId(HDEVINFO set, SP_DEVINFO_DATA& data) {
    DWORD required = 0;
    SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, nullptr, nullptr, 0, &required);
    if (required < sizeof(wchar_t)) {
        return false;
    }
    std::vector<BYTE> buffer(required + sizeof(wchar_t), 0);
    if (!SetupDiGetDeviceRegistryPropertyW(set, &data, SPDRP_HARDWAREID, nullptr, buffer.data(), required, nullptr)) {
        return false;
    }
    for (const wchar_t* id = reinterpret_cast<const wchar_t*>(buffer.data()); *id; id += wcslen(id) + 1) {
        if (_wcsicmp(id, L"Root\\ScpVBus") == 0) {
            return true;
        }
    }
    return false;
}

// Every root-enumerated device with the ScpVBus hardware ID, including leftovers that never got a driver.
Nodes findNodes() {
    Nodes nodes;
    nodes.set = DeviceSet(SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES));
    if (!nodes.set.valid()) {
        return nodes;
    }
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA data = {};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInfo(nodes.set.get(), index, &data)) {
            break;
        }
        if (hasHardwareId(nodes.set.get(), data)) {
            nodes.devices.push_back(data);
        }
    }
    return nodes;
}

// 0 when the device is healthy, otherwise the Windows problem code (52 unsigned, 39 cannot load, 10 cannot start...).
ULONG problemCode(SP_DEVINFO_DATA& data) {
    ULONG status = 0;
    ULONG problem = 0;
    return CM_Get_DevNode_Status(&status, &problem, data.DevInst, 0) == CR_SUCCESS ? problem : 0;
}

std::wstring describeProblem(ULONG problem) {
    switch (problem) {
        case CM_PROB_UNSIGNED_DRIVER:
            return L"Windows could not verify the driver's digital signature";
        case CM_PROB_DRIVER_FAILED_LOAD:
            return L"Windows could not load the driver";
        case CM_PROB_FAILED_START:
            return L"The driver was installed but could not start";
        case CM_PROB_DRIVER_BLOCKED:
            return L"Windows has blocked this driver";
        case CM_PROB_NEED_RESTART:
            return L"Windows needs a restart to finish";
        default:
            return L"The device reports a problem";
    }
}

bool driverBusPresent(FgcContext* core) {
    FgcDriverStatus status{};
    status.struct_size = sizeof(status);
    return fgc_get_driver_status(core, &status) == FGC_OK && status.scpvbus_installed != 0;
}

bool waitForBus(FgcContext* core, int seconds) {
    for (int i = 0; i < seconds * 2; ++i) {
        if (driverBusPresent(core)) {
            return true;
        }
        Sleep(500);
    }
    return driverBusPresent(core);
}

bool removeNode(HDEVINFO set, SP_DEVINFO_DATA& data, bool& restart) {
    if (!SetupDiCallClassInstaller(DIF_REMOVE, set, &data)) {
        return false;
    }
    SP_DEVINSTALL_PARAMS_W params = {};
    params.cbSize = sizeof(params);
    if (SetupDiGetDeviceInstallParamsW(set, &data, &params) && (params.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART))) {
        restart = true;
    }
    return true;
}

// Package names (oemNN.inf) in the driver store that mention the ScpVBus hardware ID. Found by reading the
// files so that a package that was staged but never attached to a device is found too.
std::vector<std::wstring> findStagedPackages() {
    std::vector<std::wstring> packages;
    wchar_t windows[MAX_PATH];
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return packages;
    }
    const std::wstring directory = std::wstring(windows, length) + L"\\INF\\";

    WIN32_FIND_DATAW found;
    const HANDLE search = FindFirstFileW((directory + L"oem*.inf").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) {
        return packages;
    }
    do {
        if (found.nFileSizeHigh != 0 || found.nFileSizeLow > 4 * 1024 * 1024) {
            continue;
        }
        const HANDLE file = CreateFileW((directory + found.cFileName).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            continue;
        }
        std::vector<char> bytes(found.nFileSizeLow);
        DWORD read = 0;
        const bool ok = bytes.empty() || ReadFile(file, bytes.data(), found.nFileSizeLow, &read, nullptr);
        CloseHandle(file);
        if (!ok) {
            continue;
        }

        std::wstring text;
        if (read >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
            text.assign(reinterpret_cast<const wchar_t*>(bytes.data() + 2), (read - 2) / sizeof(wchar_t));
        } else {
            text.resize(read);
            std::transform(bytes.begin(), bytes.begin() + read, text.begin(),
                           [](char c) { return static_cast<wchar_t>(static_cast<unsigned char>(c)); });
        }
        std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (text.find(L"root\\scpvbus") != std::wstring::npos) {
            packages.push_back(found.cFileName);
        }
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return packages;
}

void reportFailure(const wchar_t* step, DWORD error) {
    say(format(L"  Failed while %s: %s", step, describeSetupError(error).c_str()));
}

}  // namespace

std::wstring describeSetupError(unsigned long error) {
    std::wstring meaning;
    switch (error) {
        case ERROR_ACCESS_DENIED:
            meaning = L"Access denied. Run this tool as administrator.";
            break;
        case ERROR_AUTHENTICODE_DISALLOWED:
        case ERROR_AUTHENTICODE_TRUST_NOT_ESTABLISHED:
        case ERROR_AUTHENTICODE_PUBLISHER_NOT_TRUSTED:
        case ERROR_FILE_HASH_NOT_IN_CATALOG:
        case ERROR_NO_CATALOG_FOR_OEM_INF:
        case static_cast<unsigned long>(TRUST_E_NOSIGNATURE):
        case static_cast<unsigned long>(CERT_E_UNTRUSTEDROOT):
        case static_cast<unsigned long>(CERT_E_EXPIRED):
            meaning = L"Windows rejected the driver's digital signature.";
            break;
        case ERROR_DRIVER_INSTALL_BLOCKED:
        case ERROR_DEVICE_INSTALL_BLOCKED:
            meaning = L"Windows policy blocks installing this driver.";
            break;
        case ERROR_DRIVER_STORE_ADD_FAILED:
            meaning = L"Windows could not add the driver to its driver store.";
            break;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            meaning = L"A driver file is missing. Keep the drivers folder next to this tool.";
            break;
        default: {
            wchar_t* message = nullptr;
            const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                                nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
            if (length > 0 && message) {
                meaning.assign(message, length);
                while (!meaning.empty() && (meaning.back() == L'\r' || meaning.back() == L'\n' || meaning.back() == L' ')) {
                    meaning.pop_back();
                }
            } else {
                meaning = L"Unknown error.";
            }
            if (message) {
                LocalFree(message);
            }
        }
    }
    return format(L"%s (code 0x%08lX)", meaning.c_str(), error);
}

Outcome installScpVBus(const Env& env, const std::wstring& infPath) {
    say(L"ScpVBus (virtual Xbox controller):");

    if (driverBusPresent(env.core)) {
        say(L"  Already installed and running.");
        return Outcome::AlreadyDone;
    }
    if (GetFileAttributesW(infPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        say(format(L"  Cannot find %s. Keep the drivers folder next to this tool.", infPath.c_str()));
        return Outcome::Failed;
    }
    note(format(L"INF: %s", infPath.c_str()));

    GUID classGuid = {};
    wchar_t className[MAX_CLASS_NAME_LEN] = {};
    if (!SetupDiGetINFClassW(infPath.c_str(), &classGuid, className, MAX_CLASS_NAME_LEN, nullptr)) {
        reportFailure(L"reading the driver file", GetLastError());
        return Outcome::Failed;
    }

    Nodes existing = findNodes();
    const size_t leftovers = existing.devices.size();
    const bool stalePackage = !findStagedPackages().empty();
    say(format(L"  Existing ScpVBus device entries: %zu. Driver package already in Windows' driver store: %s.",
               leftovers, stalePackage ? L"yes" : L"no"));
    for (SP_DEVINFO_DATA& node : existing.devices) {
        const ULONG problem = problemCode(node);
        if (problem != 0) {
            say(format(L"  An existing entry has a problem (code %lu): %s.", problem, describeProblem(problem).c_str()));
        }
    }

    if (env.dryRun) {
        say(L"  Would: add the driver package to Windows' driver store;");
        say(leftovers > 0 ? L"  Would: reuse the existing device entry;" : L"  Would: create the Root\\ScpVBus device entry;");
        say(L"  Would: attach the driver and wait for the bus to appear. Nothing was changed (dry run).");
        return Outcome::Done;
    }

    bool restart = false;

    say(L"  Step 1 of 3: adding the driver package to Windows' driver store...");
    BOOL needReboot = FALSE;
    if (!DiInstallDriverW(nullptr, infPath.c_str(), 0, &needReboot)) {
        reportFailure(L"adding the driver package", GetLastError());
        return Outcome::Failed;
    }
    restart = restart || needReboot;

    // A leftover entry from an earlier failed attempt is reused rather than adding another.
    DeviceSet createdSet;
    SP_DEVINFO_DATA created = {};
    bool weCreatedIt = false;
    if (leftovers == 0) {
        say(L"  Step 2 of 3: creating the Root\\ScpVBus device entry...");
        createdSet = DeviceSet(SetupDiCreateDeviceInfoList(&classGuid, nullptr));
        created.cbSize = sizeof(created);
        if (!createdSet.valid() ||
            !SetupDiCreateDeviceInfoW(createdSet.get(), className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID,
                                      &created) ||
            !SetupDiSetDeviceRegistryPropertyW(createdSet.get(), &created, SPDRP_HARDWAREID,
                                               reinterpret_cast<const BYTE*>(kHardwareId), sizeof(kHardwareId)) ||
            !SetupDiCallClassInstaller(DIF_REGISTERDEVICE, createdSet.get(), &created)) {
            reportFailure(L"creating the device entry", GetLastError());
            return Outcome::Failed;
        }
        weCreatedIt = true;
    } else {
        say(L"  Step 2 of 3: reusing the existing device entry.");
    }

    say(L"  Step 3 of 3: attaching the driver...");
    needReboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, L"Root\\ScpVBus", infPath.c_str(), INSTALLFLAG_FORCE,
                                            &needReboot)) {
        const DWORD error = GetLastError();
        reportFailure(L"attaching the driver", error);
        if (weCreatedIt) {
            bool ignored = false;
            removeNode(createdSet.get(), created, ignored);  // do not leave a half-installed device behind
            say(L"  Removed the device entry that was just created.");
        }
        return Outcome::Failed;
    }
    restart = restart || needReboot;

    if (!waitForBus(env.core, 10)) {
        Nodes after = findNodes();
        for (SP_DEVINFO_DATA& node : after.devices) {
            const ULONG problem = problemCode(node);
            if (problem != 0) {
                say(format(L"  The driver was installed but the device has a problem (code %lu): %s.", problem,
                           describeProblem(problem).c_str()));
            }
        }
        if (restart) {
            say(L"  Windows asks for a restart to finish. Restart, then run this tool's status check.");
            return Outcome::RestartRequired;
        }
        say(L"  The ScpVBus device did not appear.");
        return Outcome::Failed;
    }

    say(L"  Installed and running.");
    return restart ? Outcome::RestartRequired : Outcome::Done;
}

Outcome uninstallScpVBus(const Env& env) {
    say(L"ScpVBus (virtual Xbox controller):");

    Nodes nodes = findNodes();
    const std::vector<std::wstring> packages = findStagedPackages();
    if (nodes.devices.empty() && packages.empty()) {
        say(L"  Not installed.");
        return Outcome::AlreadyDone;
    }
    say(format(L"  Found %zu device entr%s and %zu driver package%s.", nodes.devices.size(),
               nodes.devices.size() == 1 ? L"y" : L"ies", packages.size(), packages.size() == 1 ? L"" : L"s"));

    if (env.dryRun) {
        say(L"  Would: remove the device entries, then the driver packages. Nothing was changed (dry run).");
        return Outcome::Done;
    }

    bool restart = false;
    bool failed = false;
    for (SP_DEVINFO_DATA& node : nodes.devices) {
        if (!removeNode(nodes.set.get(), node, restart)) {
            reportFailure(L"removing the device", GetLastError());
            failed = true;
        }
    }
    for (const std::wstring& package : packages) {
        if (!SetupUninstallOEMInfW(package.c_str(), SUOI_FORCEDELETE, nullptr)) {
            reportFailure(format(L"removing driver package %s", package.c_str()).c_str(), GetLastError());
            failed = true;
        } else {
            note(format(L"Removed driver package %s", package.c_str()));
        }
    }

    if (failed) {
        return Outcome::Failed;
    }
    say(L"  Removed.");
    return restart ? Outcome::RestartRequired : Outcome::Done;
}

}  // namespace setup
