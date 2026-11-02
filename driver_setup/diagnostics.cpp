#include "diagnostics.h"

#include <windows.h>

#include <iostream>
#include <string>

#include "log.h"

namespace setup {
namespace {

constexpr DWORD kRestartDelaySeconds = 30;

bool readDword(const wchar_t* key, const wchar_t* value, DWORD& out) {
    DWORD size = sizeof(out);
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_DWORD, nullptr, &out, &size) == ERROR_SUCCESS;
}

std::wstring readString(const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[256] = {};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) == ERROR_SUCCESS
               ? std::wstring(buffer)
               : std::wstring();
}

// Absent on BIOS-mode PCs, which have no Secure Boot.
bool secureBootOn() {
    DWORD value = 0;
    return readDword(L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled", value) &&
           value == 1;
}

bool memoryIntegrityOn() {
    DWORD value = 0;
    return readDword(L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity",
                     L"Enabled", value) &&
           value == 1;
}

}  // namespace

void logSystemInfo() {
    constexpr wchar_t kVersionKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    note(format(L"Windows: %s %s (build %s)", readString(kVersionKey, L"ProductName").c_str(),
                readString(kVersionKey, L"DisplayVersion").c_str(), readString(kVersionKey, L"CurrentBuild").c_str()));
    note(format(L"Secure Boot: %s, memory integrity: %s", secureBootOn() ? L"on" : L"off or not available",
                memoryIntegrityOn() ? L"on" : L"off"));
}

void printFailureHelp(bool installing) {
    say(L"");
    say(L"Something went wrong.");
    if (installing) {
        if (memoryIntegrityOn()) {
            say(L"  Memory integrity (core isolation) is on. It can stop older drivers from loading. You can turn it off");
            say(L"  in Windows Security > Device security > Core isolation details, restart, and try again.");
        }
        if (secureBootOn()) {
            say(L"  Secure Boot is on. If a message above mentions a signature, it may be the cause. It can only be");
            say(L"  changed in the PC's firmware (BIOS/UEFI) settings and it lowers protection, so that is up to you.");
        }
        say(L"  Antivirus or other security software can also block driver installs; try again after allowing this tool.");
    }
    say(L"  If you ask for help, send the log file named below.");
}

void offerRestart(bool prompt) {
    if (!prompt) {
        say(L"Restart Windows when you are ready.");
        return;
    }
    say(format(L"Restart Windows now? It restarts in %lu seconds; to cancel, run 'shutdown /a'. [y/N]",
               kRestartDelaySeconds));
    std::string answer;
    std::getline(std::cin, answer);
    if (answer.empty() || (answer[0] != 'y' && answer[0] != 'Y')) {
        return;
    }

    wchar_t systemDirectory[MAX_PATH];
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    const std::wstring program = std::wstring(systemDirectory, length) + L"\\shutdown.exe";
    std::wstring commandLine = format(
        L"\"%s\" /r /t %lu /c \"FGC Keyboard Splitter driver setup: restarting to finish.\"", program.c_str(),
        kRestartDelaySeconds);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(program.c_str(), commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup, &process)) {
        say(format(L"Could not start the restart (Windows error %lu). Please restart Windows yourself.", GetLastError()));
        return;
    }
    DWORD exitCode = 1;
    WaitForSingleObject(process.hProcess, 10000);
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    if (exitCode == 0) {
        say(format(L"Windows will restart in %lu seconds. To cancel, run 'shutdown /a'.", kRestartDelaySeconds));
    } else {
        say(format(L"Could not schedule the restart (code %lu). Please restart Windows yourself.", exitCode));
    }
}

}  // namespace setup
