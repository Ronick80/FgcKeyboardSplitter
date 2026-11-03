#include "scpvbus_pad.h"

#include <windows.h>

#include <setupapi.h>
#include <winioctl.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <vector>

namespace fgc {
namespace {

// {F679F562-3164-42CE-A4DB-E7DDBE723909}
const GUID kScpVBusInterface = {0xf679f562, 0x3164, 0x42ce, {0xa4, 0xdb, 0xe7, 0xdd, 0xbe, 0x72, 0x39, 0x09}};

constexpr uint32_t kMaxPads = 4;

constexpr DWORD kIoctlPlugIn = CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x801, METHOD_BUFFERED, FILE_WRITE_DATA);
constexpr DWORD kIoctlUnplug = CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA);
constexpr DWORD kIoctlReport =
    CTL_CODE(FILE_DEVICE_BUS_EXTENDER, 0x804, METHOD_BUFFERED, FILE_WRITE_DATA | FILE_READ_DATA);

constexpr size_t kReportSize = 28;
constexpr size_t kReportGamepadOffset = 10;
constexpr size_t kFeedbackSize = 9;

std::wstring findBusPath() {
    const HDEVINFO devices =
        SetupDiGetClassDevsW(&kScpVBusInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::wstring path;
    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);
    if (SetupDiEnumDeviceInterfaces(devices, nullptr, &kScpVBusInterface, 0, &interfaceData)) {
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, &required, nullptr);
        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (required > 0 && SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, detail, required, nullptr, nullptr)) {
            path = detail->DevicePath;
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return path;
}

bool plugInSlot(HANDLE bus, uint32_t slot) {
    uint8_t buffer[16] = {};
    buffer[0] = sizeof(buffer);
    std::memcpy(&buffer[4], &slot, sizeof(slot));
    DWORD bytes = 0;
    return DeviceIoControl(bus, kIoctlPlugIn, buffer, sizeof(buffer), nullptr, 0, &bytes, nullptr) != FALSE;
}

bool unplugSlot(HANDLE bus, uint32_t slot) {
    // BUSENUM_UNPLUG_HARDWARE: size, serial number, flags, reserved.
    uint32_t buffer[4] = {sizeof(buffer), slot, 0, 0};
    DWORD bytes = 0;
    return DeviceIoControl(bus, kIoctlUnplug, buffer, sizeof(buffer), nullptr, 0, &bytes, nullptr) != FALSE;
}

// One entry per slot, so a crash handler can neutralize and unplug every live pad.
struct PadRegistration {
    std::atomic<HANDLE> bus{nullptr};
};
PadRegistration g_registeredPads[kMaxPads];

void registerPad(HANDLE bus, uint32_t slot) {
    g_registeredPads[slot - 1].bus.store(bus);
}

void unregisterPad(uint32_t slot) {
    g_registeredPads[slot - 1].bus.store(nullptr);
}

// ScpVBus unplugs every pad on the bus whenever any handle to it is closed, even one that never plugged a
// pad. So a pad hands its handle back here instead of closing it, and the handles are only closed once no
// pad of ours is left to lose.
class BusHandlePool {
public:
    // Counts the caller as a live pad until release(). INVALID_HANDLE_VALUE (with GetLastError set) on failure.
    HANDLE acquire(const std::wstring& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        HANDLE bus = INVALID_HANDLE_VALUE;
        if (!idle_.empty()) {
            bus = idle_.back();
            idle_.pop_back();
        } else {
            bus = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
        if (bus != INVALID_HANDLE_VALUE) {
            ++inUse_;
        }
        return bus;
    }

    void release(HANDLE bus) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        --inUse_;
        try {
            idle_.push_back(bus);
        } catch (...) {
            // Leaking the handle until the process exits is better than unplugging the other pads.
            return;
        }
        if (inUse_ == 0) {
            for (const HANDLE idle : idle_) {
                CloseHandle(idle);
            }
            idle_.clear();
        }
    }

private:
    std::mutex mutex_;
    std::vector<HANDLE> idle_;
    size_t inUse_ = 0;
};

BusHandlePool& busHandles() {
    static BusHandlePool* pool = new BusHandlePool;  // never destroyed, so it outlives pads freed at exit
    return *pool;
}

// Each pad keeps its own bus handle: synchronous IOCTLs on one handle are serialized, which
// would couple the latency of separate keyboards.
class ScpVBusPad final : public IVirtualPad {
public:
    ScpVBusPad(HANDLE bus, uint32_t slot) : bus_(bus), slot_(slot) {
        report_[0] = kReportSize;
        std::memcpy(&report_[4], &slot, sizeof(slot));
        report_[9] = 0x14;  // length of the gamepad payload
        registerPad(bus, slot);
    }

    ~ScpVBusPad() override {
        unregisterPad(slot_);
        reset();
        unplugSlot(bus_, slot_);
        busHandles().release(bus_);
    }

    FgcResult submit(const PadState& state) noexcept override {
        std::memcpy(&report_[kReportGamepadOffset], &state, sizeof(state));
        DWORD bytes = 0;
        return DeviceIoControl(bus_, kIoctlReport, report_, sizeof(report_), feedback_, sizeof(feedback_), &bytes,
                               nullptr)
                   ? FGC_OK
                   : FGC_ERR_IO;
    }

    void reset() noexcept override {
        submit(PadState{});
    }

private:
    HANDLE bus_;
    uint32_t slot_;
    uint8_t report_[kReportSize] = {};
    uint8_t feedback_[kFeedbackSize] = {};  // vibration and LED state; the driver requires room for it
};

// A pad left plugged by a crashed process keeps its last button state and holds its slot, so these
// handlers neutralize and unplug every live pad.
LPTOP_LEVEL_EXCEPTION_FILTER g_previousFilter = nullptr;

LONG WINAPI crashFilter(EXCEPTION_POINTERS* info) {
    releaseAllPadsNow();
    return g_previousFilter ? g_previousFilter(info) : EXCEPTION_CONTINUE_SEARCH;
}

void onTerminate() {
    releaseAllPadsNow();
    std::abort();
}

void installCrashHandlers() {
    static std::once_flag once;
    std::call_once(once, [] {
        g_previousFilter = SetUnhandledExceptionFilter(crashFilter);
        std::set_terminate(onTerminate);
    });
}

}  // namespace

void releaseAllPadsNow() noexcept {
    for (uint32_t slot = 1; slot <= kMaxPads; ++slot) {
        const HANDLE bus = g_registeredPads[slot - 1].bus.exchange(nullptr);
        if (!bus) {
            continue;
        }
        uint8_t neutral[kReportSize] = {};
        neutral[0] = kReportSize;
        std::memcpy(&neutral[4], &slot, sizeof(slot));
        neutral[9] = 0x14;
        uint8_t feedback[kFeedbackSize];
        DWORD bytes = 0;
        DeviceIoControl(bus, kIoctlReport, neutral, sizeof(neutral), feedback, sizeof(feedback), &bytes, nullptr);
        unplugSlot(bus, slot);
    }
}

bool ScpVBusPadBus::isAvailable() const {
    const HDEVINFO devices =
        SetupDiGetClassDevsW(&kScpVBusInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devices == INVALID_HANDLE_VALUE) {
        return false;
    }
    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);
    const bool present = SetupDiEnumDeviceInterfaces(devices, nullptr, &kScpVBusInterface, 0, &interfaceData) != FALSE;
    SetupDiDestroyDeviceInfoList(devices);
    return present;
}

FgcResult ScpVBusPadBus::plugIn(std::unique_ptr<IVirtualPad>& out) {
    const std::wstring path = findBusPath();
    if (path.empty()) {
        return FGC_ERR_DRIVER_MISSING;
    }
    installCrashHandlers();

    const HANDLE bus = busHandles().acquire(path);
    if (bus == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_ACCESS_DENIED ? FGC_ERR_ACCESS_DENIED : FGC_ERR_IO;
    }

    // The driver rejects a slot that is taken, so just try them in order. Its per-slot status query is
    // not used: it keeps reporting a slot as plugged for a while after an unplug.
    for (uint32_t slot = 1; slot <= kMaxPads; ++slot) {
        if (plugInSlot(bus, slot)) {
            out = std::make_unique<ScpVBusPad>(bus, slot);
            return FGC_OK;
        }
    }

    busHandles().release(bus);  // closing it here would unplug every running pad
    return FGC_ERR_CONTROLLER_LIMIT;
}

}  // namespace fgc
