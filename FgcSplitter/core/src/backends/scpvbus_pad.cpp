#include "scpvbus_pad.h"

#include <windows.h>

#include <setupapi.h>

namespace fgc {
namespace {

// {F679F562-3164-42CE-A4DB-E7DDBE723909}
const GUID kScpVBusInterface = {0xf679f562, 0x3164, 0x42ce, {0xa4, 0xdb, 0xe7, 0xdd, 0xbe, 0x72, 0x39, 0x09}};

}  // namespace

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

FgcResult ScpVBusPadBus::plugIn(std::unique_ptr<IVirtualPad>&) {
    return FGC_ERR_NOT_IMPLEMENTED;
}

}  // namespace fgc
