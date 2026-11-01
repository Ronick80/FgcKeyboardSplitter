#include "controller_counter.h"

#include <windows.h>

#include <xinput.h>

namespace fgc {

uint32_t XInputControllerCounter::connected() const {
    uint32_t count = 0;
    for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user) {
        XINPUT_STATE state;
        if (XInputGetState(user, &state) == ERROR_SUCCESS) {
            ++count;
        }
    }
    return count;
}

}  // namespace fgc
