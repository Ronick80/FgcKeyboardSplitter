#include "interception_keyboard.h"

#include <windows.h>

#include <winioctl.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
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

// The driver is used directly instead of through interception.dll. The library opens every
// device exclusively per context, so keyboards could not have independent threads, and it
// heap-allocates on every send and receive.
constexpr int kMaxKeyboards = 10;
constexpr USHORT kFilterAll = 0xFFFF;
constexpr USHORT kFilterNone = 0;
constexpr USHORT kStrokeUp = 0x01;
constexpr USHORT kStrokeE0 = 0x02;
constexpr USHORT kStrokeE1 = 0x04;
constexpr size_t kStrokesPerRead = 16;

constexpr DWORD kIoctlSetFilter = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kIoctlSetEvent = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kIoctlRead = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x840, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kIoctlWrite = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS);
constexpr DWORD kIoctlGetHardwareId = CTL_CODE(FILE_DEVICE_UNKNOWN, 0x880, METHOD_BUFFERED, FILE_ANY_ACCESS);

// KEYBOARD_INPUT_DATA from ntddkbd.h.
struct RawKeyStroke {
    USHORT unitId;
    USHORT makeCode;
    USHORT flags;
    USHORT reserved;
    ULONG extraInformation;
};
static_assert(sizeof(RawKeyStroke) == 12);

FgcResult fromWin32Error(DWORD error) {
    switch (error) {
        case ERROR_ACCESS_DENIED:
            return FGC_ERR_ACCESS_DENIED;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            return FGC_ERR_DRIVER_MISSING;
        case ERROR_SHARING_VIOLATION:
            return FGC_ERR_DEVICE_BUSY;
        default:
            return FGC_ERR_IO;
    }
}

// number is 1..10. Opened exclusively, so it fails with ERROR_SHARING_VIOLATION while in use.
HANDLE openDevice(int number, DWORD& error) {
    wchar_t name[32];
    swprintf_s(name, L"\\\\.\\interception%02d", number - 1);
    const HANDLE handle = CreateFileW(name, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    error = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    return handle;
}

std::string toUtf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

// Empty when no keyboard is attached to this slot.
std::string readHardwareId(HANDLE device) {
    wchar_t buffer[251] = {};
    DWORD bytes = 0;
    if (!DeviceIoControl(device, kIoctlGetHardwareId, nullptr, 0, buffer, 500, &bytes, nullptr) ||
        bytes < sizeof(wchar_t)) {
        return {};
    }
    return toUtf8(buffer);
}

int64_t qpcNow() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

class InterceptionKeyboardSession final : public IKeyboardSession {
public:
    ~InterceptionKeyboardSession() override {
        if (device_ != INVALID_HANDLE_VALUE) {
            USHORT filter = kFilterNone;
            DWORD bytes = 0;
            DeviceIoControl(device_, kIoctlSetFilter, &filter, sizeof(filter), nullptr, 0, &bytes, nullptr);
            CloseHandle(device_);
        }
        if (unempty_) {
            CloseHandle(unempty_);
        }
        if (stop_) {
            CloseHandle(stop_);
        }
    }

    // Starts capturing: from here on the driver holds this keyboard's strokes for us.
    // expectedHardwareId is what connected() compares the slot against.
    FgcResult init(int number, std::string expectedHardwareId) {
        expectedHardwareId_ = std::move(expectedHardwareId);
        DWORD error = 0;
        device_ = openDevice(number, error);
        if (device_ == INVALID_HANDLE_VALUE) {
            return fromWin32Error(error);
        }

        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        unempty_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_ || !unempty_) {
            return FGC_ERR_IO;
        }

        // The driver expects a zero-padded pair of handles.
        HANDLE eventArgument[2] = {unempty_, nullptr};
        DWORD bytes = 0;
        if (!DeviceIoControl(device_, kIoctlSetEvent, eventArgument, sizeof(eventArgument), nullptr, 0, &bytes,
                             nullptr)) {
            return FGC_ERR_IO;
        }
        USHORT filter = kFilterAll;
        if (!DeviceIoControl(device_, kIoctlSetFilter, &filter, sizeof(filter), nullptr, 0, &bytes, nullptr)) {
            return FGC_ERR_IO;
        }
        return FGC_OK;
    }

    WaitResult wait(KeyEvent& out) noexcept override {
        for (;;) {
            while (next_ < count_) {
                const RawKeyStroke& stroke = strokes_[next_++];
                if (stroke.flags & kStrokeE1) {
                    continue;
                }
                const unsigned extended = (stroke.flags & kStrokeE0) ? FGC_KEY_EXTENDED_FLAG : 0u;
                out.keyCode = static_cast<uint16_t>((stroke.makeCode & 0xFFu) | extended);
                out.down = (stroke.flags & kStrokeUp) == 0;
                out.timestampQpc = wokeQpc_;
                return WaitResult::Event;
            }

            // stop_ first so a stop request wins over pending input.
            const HANDLE handles[2] = {stop_, unempty_};
            const DWORD waited = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (waited == WAIT_OBJECT_0) {
                return WaitResult::Interrupted;
            }
            if (waited != WAIT_OBJECT_0 + 1) {
                return WaitResult::DeviceLost;
            }
            wokeQpc_ = qpcNow();

            DWORD bytes = 0;
            if (!DeviceIoControl(device_, kIoctlRead, nullptr, 0, strokes_, sizeof(strokes_), &bytes, nullptr)) {
                return WaitResult::DeviceLost;
            }
            count_ = bytes / sizeof(RawKeyStroke);
            next_ = 0;
        }
    }

    void interrupt() noexcept override {
        SetEvent(stop_);
    }

    bool connected() const noexcept override {
        try {
            // The slot keeps its number but reports no hardware ID (or another one) once the keyboard is gone.
            return readHardwareId(device_) == expectedHardwareId_;
        } catch (...) {
            return true;  // a failed probe must not be mistaken for an unplugged keyboard
        }
    }

    void releaseKeys(const uint16_t* keyCodes, size_t count) noexcept override {
        for (size_t first = 0; first < count; first += kStrokesPerRead) {
            RawKeyStroke strokes[kStrokesPerRead] = {};
            const size_t batch = count - first < kStrokesPerRead ? count - first : kStrokesPerRead;
            for (size_t i = 0; i < batch; ++i) {
                const uint16_t code = keyCodes[first + i];
                strokes[i].makeCode = static_cast<USHORT>(code & 0xFFu);
                strokes[i].flags = static_cast<USHORT>(kStrokeUp | ((code & FGC_KEY_EXTENDED_FLAG) ? kStrokeE0 : 0));
            }
            DWORD bytes = 0;
            DeviceIoControl(device_, kIoctlWrite, strokes, static_cast<DWORD>(batch * sizeof(RawKeyStroke)), nullptr, 0,
                            &bytes, nullptr);
        }
    }

private:
    HANDLE device_ = INVALID_HANDLE_VALUE;
    HANDLE unempty_ = nullptr;  // driver signals it while strokes are queued
    HANDLE stop_ = nullptr;
    std::string expectedHardwareId_;
    RawKeyStroke strokes_[kStrokesPerRead] = {};
    size_t count_ = 0;
    size_t next_ = 0;
    int64_t wokeQpc_ = 0;
};

struct IdentifyTarget {
    int number;
    std::string hardwareId;
};

// Captures every idle keyboard only to see which one is pressed, handing each stroke straight
// back to Windows so typing keeps working meanwhile.
class InterceptionIdentifySession final : public IIdentifySession {
public:
    ~InterceptionIdentifySession() override {
        if (stop_) {
            SetEvent(stop_);
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (stop_) {
            CloseHandle(stop_);
        }
    }

    // Keyboards that are already captured by a running session are skipped.
    FgcResult start(const std::vector<IdentifyTarget>& targets) {
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) {
            return FGC_ERR_IO;
        }
        for (const IdentifyTarget& target : targets) {
            Source source;
            source.number = target.number;
            source.hardwareId = target.hardwareId;
            if (capture(source)) {
                sources_.push_back(std::move(source));
            } else {
                release(source);
            }
        }
        if (sources_.empty()) {
            return targets.empty() ? FGC_ERR_NOT_FOUND : FGC_ERR_DEVICE_BUSY;
        }
        thread_ = std::thread([this] { run(); });
        return FGC_OK;
    }

    bool poll(std::string& hardwareId) override {
        const int number = identified_.load(std::memory_order_acquire);
        if (number == 0) {
            return false;
        }
        for (const Source& source : sources_) {
            if (source.number == number) {
                hardwareId = source.hardwareId;
                return true;
            }
        }
        return false;
    }

private:
    struct Source {
        int number = 0;
        std::string hardwareId;
        HANDLE device = INVALID_HANDLE_VALUE;
        HANDLE unempty = nullptr;
    };

    static bool capture(Source& source) {
        DWORD error = 0;
        source.device = openDevice(source.number, error);
        if (source.device == INVALID_HANDLE_VALUE) {
            return false;
        }
        source.unempty = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!source.unempty) {
            return false;
        }
        HANDLE eventArgument[2] = {source.unempty, nullptr};
        USHORT filter = kFilterAll;
        DWORD bytes = 0;
        return DeviceIoControl(source.device, kIoctlSetEvent, eventArgument, sizeof(eventArgument), nullptr, 0,
                               &bytes, nullptr) &&
               DeviceIoControl(source.device, kIoctlSetFilter, &filter, sizeof(filter), nullptr, 0, &bytes, nullptr);
    }

    static void release(Source& source) {
        if (source.device != INVALID_HANDLE_VALUE) {
            USHORT filter = kFilterNone;
            DWORD bytes = 0;
            DeviceIoControl(source.device, kIoctlSetFilter, &filter, sizeof(filter), nullptr, 0, &bytes, nullptr);
            CloseHandle(source.device);
            source.device = INVALID_HANDLE_VALUE;
        }
        if (source.unempty) {
            CloseHandle(source.unempty);
            source.unempty = nullptr;
        }
    }

    void run() {
        std::vector<HANDLE> handles;
        handles.push_back(stop_);
        for (const Source& source : sources_) {
            handles.push_back(source.unempty);
        }

        RawKeyStroke strokes[kStrokesPerRead];
        bool found = false;
        while (!found) {
            const DWORD waited =
                WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
            if (waited == WAIT_OBJECT_0 || waited >= WAIT_OBJECT_0 + handles.size()) {
                break;
            }
            const Source& source = sources_[waited - WAIT_OBJECT_0 - 1];

            DWORD bytes = 0;
            if (!DeviceIoControl(source.device, kIoctlRead, nullptr, 0, strokes, sizeof(strokes), &bytes, nullptr)) {
                break;
            }
            const size_t count = bytes / sizeof(RawKeyStroke);
            if (count == 0) {
                continue;
            }
            DeviceIoControl(source.device, kIoctlWrite, strokes, static_cast<DWORD>(count * sizeof(RawKeyStroke)),
                            nullptr, 0, &bytes, nullptr);

            for (size_t i = 0; i < count; ++i) {
                if ((strokes[i].flags & (kStrokeUp | kStrokeE1)) == 0) {
                    identified_.store(source.number, std::memory_order_release);
                    found = true;
                    break;
                }
            }
        }

        for (Source& source : sources_) {
            release(source);
        }
    }

    HANDLE stop_ = nullptr;
    std::vector<Source> sources_;  // fixed once the thread starts, apart from handles the thread closes
    std::atomic<int> identified_{0};  // Interception keyboard number of the first key press, 0 if none
    std::thread thread_;
};

}  // namespace

DriverState InterceptionKeyboardInput::driverState() const {
    if (!isOblitaKeyboardDriverFile()) {
        return DriverState::NotInstalled;
    }
    return isKeyboardServiceKnownToScm() ? DriverState::Installed : DriverState::RebootRequired;
}

FgcResult InterceptionKeyboardInput::enumerate(std::vector<KeyboardDevice>& out) {
    std::vector<Known> found;
    DWORD lastError = ERROR_SUCCESS;
    bool reachable = false;

    for (int number = 1; number <= kMaxKeyboards; ++number) {
        DWORD error = 0;
        const HANDLE device = openDevice(number, error);
        if (device == INVALID_HANDLE_VALUE) {
            if (error != ERROR_SHARING_VIOLATION) {
                lastError = error;
                continue;
            }
            reachable = true;
            for (const Known& known : known_) {
                if (known.deviceNumber == number) {
                    found.push_back(known);
                    break;
                }
            }
            continue;
        }

        reachable = true;
        std::string rawId = readHardwareId(device);
        CloseHandle(device);
        if (!rawId.empty()) {
            Known known;
            known.deviceNumber = number;
            known.rawHardwareId = std::move(rawId);
            found.push_back(std::move(known));
        }
    }

    if (!reachable) {
        return fromWin32Error(lastError);
    }

    std::map<std::string, int> occurrences;
    for (Known& known : found) {
        const int count = ++occurrences[known.rawHardwareId];
        known.hardwareId = count == 1 ? known.rawHardwareId : known.rawHardwareId + "#" + std::to_string(count);
    }

    known_ = std::move(found);
    out.clear();
    for (const Known& known : known_) {
        out.push_back({known.hardwareId, "Keyboard " + std::to_string(known.deviceNumber) + " - " + known.rawHardwareId});
    }
    return FGC_OK;
}

FgcResult InterceptionKeyboardInput::open(const std::string& hardwareId, std::unique_ptr<IKeyboardSession>& out) {
    // Device numbers can change when keyboards are replugged, so always rescan first.
    std::vector<KeyboardDevice> scanned;
    const FgcResult scanResult = enumerate(scanned);
    if (scanResult != FGC_OK) {
        return scanResult;
    }

    int number = 0;
    std::string rawHardwareId;
    for (const Known& known : known_) {
        if (known.hardwareId == hardwareId) {
            number = known.deviceNumber;
            rawHardwareId = known.rawHardwareId;
            break;
        }
    }
    if (number == 0) {
        return FGC_ERR_NOT_FOUND;
    }

    auto session = std::make_unique<InterceptionKeyboardSession>();
    const FgcResult result = session->init(number, std::move(rawHardwareId));
    if (result != FGC_OK) {
        return result;
    }
    out = std::move(session);
    return FGC_OK;
}

FgcResult InterceptionKeyboardInput::beginIdentify(std::unique_ptr<IIdentifySession>& out) {
    std::vector<KeyboardDevice> scanned;
    const FgcResult scanResult = enumerate(scanned);
    if (scanResult != FGC_OK) {
        return scanResult;
    }

    std::vector<IdentifyTarget> targets;
    for (const Known& known : known_) {
        targets.push_back({known.deviceNumber, known.hardwareId});
    }

    auto session = std::make_unique<InterceptionIdentifySession>();
    const FgcResult result = session->start(targets);
    if (result != FGC_OK) {
        return result;
    }
    out = std::move(session);
    return FGC_OK;
}

}  // namespace fgc
