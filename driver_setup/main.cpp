#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

#include "diagnostics.h"
#include "fgc_core.h"
#include "fgc_version.h"
#include "interception_installer.h"
#include "log.h"
#include "scpvbus_installer.h"

namespace {

using setup::Outcome;

struct Options {
    std::wstring command;  // status, install, uninstall; empty means the interactive menu
    bool scpvbus = false;
    bool interception = false;
    bool yes = false;
    bool dryRun = false;
    bool help = false;
};

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != FALSE;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring lower(std::wstring text) {
    for (wchar_t& c : text) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return text;
}

void printUsage() {
    setup::say(
        L"usage:\r\n"
        L"  FgcDriverSetup                         menu\r\n"
        L"  FgcDriverSetup status                  show which drivers are installed\r\n"
        L"  FgcDriverSetup install [driver]        install missing drivers\r\n"
        L"  FgcDriverSetup uninstall <driver>      remove a driver\r\n"
        L"driver is scpvbus, interception or all (the default for install).\r\n"
        L"options: --yes (do not ask), --dry-run (show what would happen, change nothing)");
}

bool parse(int argc, wchar_t** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = lower(argv[i]);
        if (arg == L"--yes" || arg == L"-y") {
            options.yes = true;
        } else if (arg == L"--dry-run") {
            options.dryRun = true;
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            options.help = true;
        } else if (options.command.empty() && (arg == L"status" || arg == L"install" || arg == L"uninstall")) {
            options.command = arg;
        } else if (arg == L"scpvbus") {
            options.scpvbus = true;
        } else if (arg == L"interception") {
            options.interception = true;
        } else if (arg == L"all") {
            options.scpvbus = options.interception = true;
        } else {
            setup::say(setup::format(L"Unknown argument: %s", argv[i]));
            return false;
        }
    }
    return true;
}

FgcDriverStatus printStatus(FgcContext* core) {
    FgcDriverStatus status{};
    status.struct_size = sizeof(status);
    if (fgc_get_driver_status(core, &status) != FGC_OK) {
        setup::say(L"Could not read the driver status.");
        return status;
    }
    const wchar_t* interception = status.interception == FGC_INTERCEPTION_INSTALLED ? L"installed"
                                  : status.interception == FGC_INTERCEPTION_REBOOT_REQUIRED
                                      ? L"installed, restart Windows to finish"
                                      : L"not installed";
    setup::say(L"Driver status:");
    setup::say(setup::format(L"  Interception (keyboard capture):  %s", interception));
    setup::say(setup::format(L"  ScpVBus (virtual Xbox controller): %s",
                             status.scpvbus_installed ? L"installed" : L"not installed"));
    return status;
}

bool confirm(const Options& options, const std::wstring& question) {
    if (options.yes || options.dryRun) {
        return true;
    }
    setup::say(question + L" [y/N]");
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer[0] == 'y' || answer[0] == 'Y');
}

std::wstring scpvbusInfPath() {
    return setup::programDirectory() + L"\\drivers\\ScpVBus\\ScpVBus.inf";
}

// Exit code: 1 if anything failed, 3 if only a restart is pending, otherwise 0.
int exitCodeFor(const std::vector<Outcome>& outcomes) {
    bool restart = false;
    for (const Outcome outcome : outcomes) {
        if (outcome == Outcome::Failed || outcome == Outcome::Unsupported) {
            return 1;
        }
        restart = restart || outcome == Outcome::RestartRequired;
    }
    return restart ? 3 : 0;
}

int runInstall(const setup::Env& env, const Options& options) {
    // With no driver named, install whichever is missing.
    const bool both = !options.scpvbus && !options.interception;
    std::vector<Outcome> outcomes;
    printStatus(env.core);
    setup::say(L"");
    if (both || options.scpvbus) {
        outcomes.push_back(setup::installScpVBus(env, scpvbusInfPath()));
    }
    if (both || options.interception) {
        outcomes.push_back(setup::installInterception(env));
    }

    const int code = exitCodeFor(outcomes);
    setup::say(L"");
    if (code == 1) {
        setup::printFailureHelp(true);
    } else if (code == 3) {
        setup::say(L"Restart Windows to finish, then run this tool again with 'status' to confirm.");
        setup::offerRestart(!options.yes && !options.dryRun);
    }
    return code;
}

int runUninstall(const setup::Env& env, const Options& options) {
    if (!options.scpvbus && !options.interception) {
        setup::say(L"Say which driver to remove: scpvbus or interception.");
        return 2;
    }
    std::vector<Outcome> outcomes;
    if (options.scpvbus) {
        if (confirm(options, L"Remove the ScpVBus driver? Other programs that use it (for example ScpToolkit) stop working.")) {
            outcomes.push_back(setup::uninstallScpVBus(env));
        }
    }
    if (options.interception) {
        if (confirm(options, L"Remove the Interception driver? FGC Keyboard Splitter stops working until it is installed again.")) {
            outcomes.push_back(setup::uninstallInterception(env));
        }
    }
    const int code = exitCodeFor(outcomes);
    if (code == 1) {
        setup::printFailureHelp(false);
    } else if (code == 3) {
        setup::say(L"Restart Windows to finish.");
        setup::offerRestart(!options.yes && !options.dryRun);
    }
    return code;
}

int runMenu(const setup::Env& env) {
    for (;;) {
        setup::say(L"");
        printStatus(env.core);
        setup::say(L"");
        setup::say(L"  1) Install missing drivers");
        setup::say(L"  2) Remove a driver");
        setup::say(L"  3) Check status again");
        setup::say(L"  Q) Quit");
        std::string choice;
        std::getline(std::cin, choice);
        if (choice.empty()) {
            continue;
        }

        Options options;
        options.dryRun = env.dryRun;
        if (choice[0] == '1') {
            runInstall(env, options);
        } else if (choice[0] == '2') {
            setup::say(L"Remove which driver?  1) ScpVBus   2) Interception");
            std::string which;
            std::getline(std::cin, which);
            options.scpvbus = !which.empty() && which[0] == '1';
            options.interception = !which.empty() && which[0] == '2';
            runUninstall(env, options);
        } else if (choice[0] == 'q' || choice[0] == 'Q') {
            return 0;
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    setup::openLog();
    setup::logSystemInfo();
    setup::say(setup::format(L"%S %S - driver setup", FGC_PROJECT_NAME, FGC_VERSION_STRING));

    Options options;
    if (!parse(argc, argv, options) || options.help) {
        printUsage();
        return options.help ? 0 : 2;
    }

    const bool changesSystem = (options.command == L"install" || options.command == L"uninstall" || options.command.empty()) &&
                               !options.dryRun;
    if (changesSystem && !isElevated()) {
        setup::say(L"This tool installs drivers, so it must run as administrator. Right-click it and choose");
        setup::say(L"\"Run as administrator\", or use --dry-run to only see what it would do.");
        return 1;
    }

    FgcContext* core = nullptr;
    if (fgc_create(&core) != FGC_OK) {
        setup::say(L"Could not load the FgcCore library. Keep FgcCore.dll next to this tool.");
        return 1;
    }

    setup::Env env;
    env.core = core;
    env.dryRun = options.dryRun;

    int code = 0;
    if (options.command == L"status") {
        printStatus(core);
    } else if (options.command == L"install") {
        code = runInstall(env, options);
    } else if (options.command == L"uninstall") {
        code = runUninstall(env, options);
    } else {
        code = runMenu(env);
    }

    fgc_destroy(core);
    setup::say(setup::format(L"Log: %s", setup::logPath().c_str()));
    return code;
}
