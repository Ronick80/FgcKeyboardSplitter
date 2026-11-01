// Minimal command-line front end for FgcCore. It uses only the public C API, the same way another
// UI (C#, Qt, ...) would.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fgc_core.h"
#include "fgc_version.h"

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI onConsoleEvent(DWORD) {
    g_stop = true;
    Sleep(5000);  // the process is ended when this returns, so give main time to stop the sessions
    return TRUE;
}

const char* describe(FgcResult result) {
    switch (result) {
        case FGC_OK:
            return "ok";
        case FGC_ERR_DRIVER_MISSING:
            return "a driver is missing or not active yet";
        case FGC_ERR_ALREADY_RUNNING:
            return "already running";
        case FGC_ERR_CONTROLLER_LIMIT:
            return "no free controller slot";
        case FGC_ERR_NOT_FOUND:
            return "not found";
        case FGC_ERR_ACCESS_DENIED:
            return "access denied";
        case FGC_ERR_DEVICE_BUSY:
            return "keyboard is in use by another program";
        default:
            return "error";
    }
}

std::vector<FgcKeyboardInfo> listKeyboards(FgcContext* core) {
    std::vector<FgcKeyboardInfo> keyboards;
    if (fgc_refresh_keyboards(core) != FGC_OK) {
        return keyboards;
    }
    uint32_t count = 0;
    fgc_get_keyboard_count(core, &count);
    for (uint32_t i = 0; i < count; ++i) {
        FgcKeyboardInfo info{};
        info.struct_size = sizeof(info);
        if (fgc_get_keyboard(core, i, &info) == FGC_OK) {
            keyboards.push_back(info);
        }
    }
    return keyboards;
}

void printUsage() {
    std::puts(
        "usage:\n"
        "  FgcCli version\n"
        "  FgcCli list\n"
        "      Show connected keyboards and driver status.\n"
        "  FgcCli run <index|hardware-id> [seconds]\n"
        "      Map that keyboard to a virtual Xbox controller until Ctrl+C, or for the given number of\n"
        "      seconds. The keyboard is hidden from other programs meanwhile, so stop with Ctrl+C typed on\n"
        "      a different keyboard, or pass a number of seconds.");
}

int list(FgcContext* core) {
    FgcDriverStatus drivers{};
    drivers.struct_size = sizeof(drivers);
    fgc_get_driver_status(core, &drivers);
    std::printf("Interception: %s\n", drivers.interception == FGC_INTERCEPTION_INSTALLED ? "installed"
                                       : drivers.interception == FGC_INTERCEPTION_REBOOT_REQUIRED ? "restart required"
                                                                                                    : "not installed");
    std::printf("ScpVBus:      %s\n", drivers.scpvbus_installed ? "installed" : "not installed");

    const std::vector<FgcKeyboardInfo> keyboards = listKeyboards(core);
    if (keyboards.empty()) {
        std::puts("No keyboards found.");
    }
    for (size_t i = 0; i < keyboards.size(); ++i) {
        std::printf("[%zu] %s\n", i, keyboards[i].display_name);
    }
    return 0;
}

int run(FgcContext* core, const char* selector, int seconds) {
    const std::vector<FgcKeyboardInfo> keyboards = listKeyboards(core);
    const FgcKeyboardInfo* chosen = nullptr;
    char* end = nullptr;
    const unsigned long index = std::strtoul(selector, &end, 10);
    if (*selector != '\0' && *end == '\0' && index < keyboards.size()) {
        chosen = &keyboards[index];
    }
    for (const FgcKeyboardInfo& keyboard : keyboards) {
        if (!chosen && std::strcmp(keyboard.hardware_id, selector) == 0) {
            chosen = &keyboard;
        }
    }
    if (!chosen) {
        std::fprintf(stderr, "No keyboard matches '%s'. Run 'FgcCli list'.\n", selector);
        return 2;
    }

    const FgcResult started = fgc_start(core, chosen->hardware_id);
    if (started != FGC_OK) {
        std::fprintf(stderr, "Start failed: %s\n", describe(started));
        return 1;
    }
    std::printf("Running %s\n", chosen->display_name);

    const ULONGLONG begin = GetTickCount64();
    while (!g_stop && (seconds <= 0 || GetTickCount64() - begin < static_cast<ULONGLONG>(seconds) * 1000)) {
        FgcEvent event{};
        event.struct_size = sizeof(event);
        while (fgc_poll_event(core, &event) == FGC_OK && event.type != FGC_EVENT_NONE) {
            std::printf("Event: %s\n", event.type == FGC_EVENT_KEYBOARD_DISCONNECTED ? "keyboard unplugged"
                                                                                       : "session faulted");
            g_stop = true;
        }
        Sleep(100);
    }

    FgcLatencyStats stats{};
    stats.struct_size = sizeof(stats);
    if (fgc_get_latency_stats(core, chosen->hardware_id, &stats) == FGC_OK && stats.sample_count > 0) {
        std::printf("Latency over %llu events: average %.1f us, max %.1f us\n",
                    static_cast<unsigned long long>(stats.sample_count), stats.average_ns / 1000.0,
                    stats.max_ns / 1000.0);
    }

    fgc_stop(core, chosen->hardware_id);
    std::puts("Stopped.");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 2;
    }

    FgcContext* core = nullptr;
    if (fgc_create(&core) != FGC_OK) {
        std::fprintf(stderr, "Could not initialize FgcCore.\n");
        return 1;
    }
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);

    int result = 2;
    if (std::strcmp(argv[1], "list") == 0) {
        result = list(core);
    } else if (std::strcmp(argv[1], "version") == 0 || std::strcmp(argv[1], "--version") == 0) {
        std::printf("%s %s\n", FGC_PROJECT_NAME, fgc_get_version());
        result = 0;
    } else if (std::strcmp(argv[1], "run") == 0 && argc >= 3) {
        result = run(core, argv[2], argc >= 4 ? std::atoi(argv[3]) : 0);
    } else {
        printUsage();
    }

    fgc_destroy(core);
    return result;
}
