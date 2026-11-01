#include "interception_keyboard.h"

#include <windows.h>

#include <string>
#include <vector>

namespace fgc {
namespace {

bool isOblitaKeyboardDriverFile() {
    wchar_t systemDir[MAX_PATH];
    const UINT length = GetSystemDirectoryW(systemDir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    const std::wstring path = std::wstring(systemDir) + L"\\drivers\\keyboard.sys";

    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (size == 0) {
        return false;
    }
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return false;
    }

    struct LangCodePage {
        WORD language;
        WORD codePage;
    };
    LangCodePage* translations = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<LPVOID*>(&translations),
                        &translationBytes) ||
        translationBytes < sizeof(LangCodePage)) {
        return false;
    }

    const auto readString = [&](const wchar_t* name) -> std::wstring {
        wchar_t subBlock[96];
        swprintf_s(subBlock, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language,
                   translations[0].codePage, name);
        wchar_t* value = nullptr;
        UINT valueLength = 0;
        if (!VerQueryValueW(data.data(), subBlock, reinterpret_cast<LPVOID*>(&value), &valueLength) || !value) {
            return {};
        }
        return value;
    };

    return readString(L"CompanyName") == L"Oblita" &&
           readString(L"FileDescription") == L"Keyboard Upper Filter Driver";
}

// Interception writes its service key directly to the registry, so the SCM only knows
// about it after a reboot.
bool isKeyboardServiceKnownToScm() {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        return false;
    }
    const SC_HANDLE service = OpenServiceW(scm, L"keyboard", SERVICE_QUERY_STATUS);
    const bool known = service != nullptr;
    if (service) {
        CloseServiceHandle(service);
    }
    CloseServiceHandle(scm);
    return known;
}

}  // namespace

DriverState InterceptionKeyboardInput::driverState() const {
    if (!isOblitaKeyboardDriverFile()) {
        return DriverState::NotInstalled;
    }
    return isKeyboardServiceKnownToScm() ? DriverState::Installed : DriverState::RebootRequired;
}

FgcResult InterceptionKeyboardInput::enumerate(std::vector<KeyboardDevice>&) {
    return FGC_ERR_NOT_IMPLEMENTED;
}

FgcResult InterceptionKeyboardInput::open(const std::string&, std::unique_ptr<IKeyboardSession>&) {
    return FGC_ERR_NOT_IMPLEMENTED;
}

}  // namespace fgc
