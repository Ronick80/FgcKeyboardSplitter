#include "interception_installer.h"

#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>
#include <shlobj.h>
#include <winhttp.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "log.h"

namespace setup {
namespace {

// Official oblitum/Interception v1.0.1 release. The installer inside is unsigned, so this hash is the trust anchor.
constexpr wchar_t kDownloadHost[] = L"github.com";
constexpr wchar_t kDownloadPath[] = L"/oblitum/Interception/releases/download/v1.0.1/Interception.zip";
constexpr char kZipSha256[] = "ad038963d6413055765128b0b931f6e765147c9916dba79e65d872b261f9af10";
constexpr wchar_t kReleasePage[] = L"https://github.com/oblitum/Interception/releases/tag/v1.0.1";
constexpr wchar_t kInstallerInZip[] = L"Interception/command line installer/install-interception.exe";
constexpr size_t kMaxZipBytes = 16u * 1024u * 1024u;

using Bytes = std::vector<uint8_t>;

std::string sha256Hex(const Bytes& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    uint8_t digest[32] = {};
    const NTSTATUS status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(data.data()),
                                       static_cast<ULONG>(data.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) {
        return {};
    }
    static const char digits[] = "0123456789abcdef";
    std::string hex;
    for (const uint8_t byte : digest) {
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

bool hashMatches(const Bytes& data) {
    return sha256Hex(data) == kZipSha256;
}

bool readFile(const std::wstring& path, Bytes& out) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart <= static_cast<LONGLONG>(kMaxZipBytes);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
    }
    CloseHandle(file);
    return ok;
}

bool writeNewFile(const std::wstring& path, const Bytes& data) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                    written == data.size();
    CloseHandle(file);
    return ok;
}

struct InternetHandle {
    HINTERNET handle = nullptr;
    explicit InternetHandle(HINTERNET value) : handle(value) {}
    ~InternetHandle() {
        if (handle != nullptr) {
            WinHttpCloseHandle(handle);
        }
    }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
};

bool download(Bytes& out, std::wstring& failure) {
    const auto fail = [&](const wchar_t* what, DWORD error) {
        failure = format(L"%s (Windows error %lu)", what, error);
        return false;
    };

    InternetHandle session(WinHttpOpen(L"FgcDriverSetup", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                       WINHTTP_NO_PROXY_BYPASS, 0));
    if (session.handle == nullptr) {
        return fail(L"Could not start the download", GetLastError());
    }
    WinHttpSetTimeouts(session.handle, 15000, 15000, 30000, 30000);

    InternetHandle connection(WinHttpConnect(session.handle, kDownloadHost, INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connection.handle == nullptr) {
        return fail(L"Could not reach github.com", GetLastError());
    }
    InternetHandle request(WinHttpOpenRequest(connection.handle, L"GET", kDownloadPath, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (request.handle == nullptr) {
        return fail(L"Could not start the download", GetLastError());
    }
    if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.handle, nullptr)) {
        return fail(L"Could not connect to github.com", GetLastError());
    }

    DWORD httpStatus = 0;
    DWORD statusSize = sizeof(httpStatus);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &httpStatus, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        return fail(L"Unreadable server answer", GetLastError());
    }
    if (httpStatus != 200) {
        failure = format(L"The server answered HTTP %lu", httpStatus);
        return false;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.handle, &available)) {
            return fail(L"The download was interrupted", GetLastError());
        }
        if (available == 0) {
            return true;
        }
        if (out.size() + available > kMaxZipBytes) {
            failure = L"The file is larger than expected";
            return false;
        }
        const size_t offset = out.size();
        out.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.handle, out.data() + offset, available, &read)) {
            return fail(L"The download was interrupted", GetLastError());
        }
        out.resize(offset + read);
    }
}

// The verified zip comes from a copy next to this tool when there is one, otherwise from GitHub.
bool obtainVerifiedZip(Bytes& zip) {
    const std::wstring local = programDirectory() + L"\\Interception.zip";
    Bytes candidate;
    if (readFile(local, candidate)) {
        if (hashMatches(candidate)) {
            say(L"  Using Interception.zip found next to this tool (checksum verified).");
            zip = std::move(candidate);
            return true;
        }
        say(L"  Ignoring Interception.zip next to this tool: it is not the expected v1.0.1 file.");
    }

    say(L"  Downloading Interception v1.0.1 (about 380 KB) from github.com...");
    Bytes downloaded;
    std::wstring failure;
    if (!download(downloaded, failure)) {
        say(L"  Download failed: " + failure + L".");
        say(L"  Download Interception.zip yourself from " + std::wstring(kReleasePage));
        say(L"  and put it next to this tool, then run the tool again.");
        return false;
    }
    if (!hashMatches(downloaded)) {
        say(L"  The downloaded file does not match the expected checksum, so it was NOT used.");
        say(L"  Try again later, or download Interception.zip yourself from " + std::wstring(kReleasePage));
        say(L"  and put it next to this tool. Expected SHA-256: " + toWide(kZipSha256));
        return false;
    }
    say(L"  Download complete, checksum verified.");
    zip = std::move(downloaded);
    return true;
}

// Created with an unpredictable name and an administrators-only ACL so a normal user cannot swap files we then run.
bool createPrivateDirectory(std::wstring& path, std::wstring& failure) {
    PWSTR programData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) {
        failure = L"Could not find the ProgramData folder";
        return false;
    }
    const std::wstring base = programData;
    CoTaskMemFree(programData);

    uint8_t random[8] = {};
    if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        failure = L"Could not create a random folder name";
        return false;
    }
    std::wstring name = L"FgcSetup-";
    for (const uint8_t byte : random) {
        name += format(L"%02x", byte);
    }
    path = base + L"\\" + name;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
                                                              SDDL_REVISION_1, &descriptor, nullptr)) {
        failure = format(L"Could not prepare folder permissions (Windows error %lu)", GetLastError());
        return false;
    }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), descriptor, FALSE};
    const bool created = CreateDirectoryW(path.c_str(), &attributes) != FALSE;
    const DWORD error = GetLastError();
    LocalFree(descriptor);
    if (!created) {
        failure = format(L"Could not create a working folder (Windows error %lu)", error);
    }
    return created;
}

struct DirectoryCleanup {
    std::wstring path;
    ~DirectoryCleanup() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

struct RunResult {
    bool started = false;
    DWORD startError = 0;
    bool timedOut = false;
    DWORD exitCode = 0;
    std::string output;
};

RunResult runProcess(const std::wstring& application, const std::wstring& arguments, DWORD timeoutMs) {
    RunResult result;
    SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), nullptr, TRUE};
    HANDLE readEnd = nullptr;
    HANDLE writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) {
        result.startError = GetLastError();
        return result;
    }
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    const HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = nul;
    startup.hStdOutput = writeEnd;
    startup.hStdError = writeEnd;

    std::wstring commandLine = L"\"" + application + L"\" " + arguments;
    PROCESS_INFORMATION process = {};
    const BOOL created = CreateProcessW(application.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    result.startError = created ? 0 : GetLastError();
    CloseHandle(writeEnd);
    if (nul != INVALID_HANDLE_VALUE) {
        CloseHandle(nul);
    }
    if (!created) {
        CloseHandle(readEnd);
        return result;
    }
    result.started = true;

    const auto drain = [&]() {
        char buffer[4096];
        DWORD available = 0;
        while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD read = 0;
            if (!ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) || read == 0) {
                break;
            }
            result.output.append(buffer, read);
        }
    };

    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        drain();
        if (WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0) {
            drain();
            break;
        }
        if (GetTickCount64() > deadline) {
            result.timedOut = true;
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 5000);
            break;
        }
    }
    GetExitCodeProcess(process.hProcess, &result.exitCode);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    CloseHandle(readEnd);
    return result;
}

void sayProgramOutput(const std::string& output) {
    const int size = MultiByteToWideChar(CP_OEMCP, 0, output.data(), static_cast<int>(output.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(size), L'\0');
    if (size > 0) {
        MultiByteToWideChar(CP_OEMCP, 0, output.data(), static_cast<int>(output.size()), text.data(), size);
    }
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == std::wstring::npos) {
            end = text.size();
        }
        std::wstring line = text.substr(start, end - start);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            say(L"    " + line);
        }
        start = end + 1;
    }
}

FgcInterceptionState currentState(const Env& env) {
    FgcDriverStatus status = {};
    status.struct_size = sizeof(status);
    if (fgc_get_driver_status(env.core, &status) != FGC_OK) {
        return FGC_INTERCEPTION_NOT_INSTALLED;
    }
    return status.interception;
}

// Downloads and unpacks the installer into a private folder, runs it with the given switch, and cleans up.
Outcome runReleaseInstaller(const Env& env, const wchar_t* verb, const wchar_t* argument, bool installing) {
    Bytes zip;
    if (!obtainVerifiedZip(zip)) {
        return Outcome::Failed;
    }

    DirectoryCleanup workFolder;
    std::wstring failure;
    if (!createPrivateDirectory(workFolder.path, failure)) {
        workFolder.path.clear();
        say(L"  " + failure + L".");
        return Outcome::Failed;
    }
    note(L"Working folder: " + workFolder.path);

    const std::wstring zipPath = workFolder.path + L"\\Interception.zip";
    if (!writeNewFile(zipPath, zip)) {
        say(L"  Could not write the downloaded file to the working folder.");
        return Outcome::Failed;
    }

    wchar_t systemDirectory[MAX_PATH];
    const UINT length = GetSystemDirectoryW(systemDirectory, MAX_PATH);
    const std::wstring tar = std::wstring(systemDirectory, length) + L"\\tar.exe";
    const RunResult unpack = runProcess(
        tar, L"-xf \"" + zipPath + L"\" -C \"" + workFolder.path + L"\" \"" + kInstallerInZip + L"\"", 60000);
    const std::wstring installer =
        std::filesystem::path(workFolder.path + L"\\" + kInstallerInZip).make_preferred().wstring();
    if (!unpack.started || unpack.exitCode != 0 || !std::filesystem::exists(installer)) {
        say(L"  Could not unpack the installer (it needs tar.exe, which comes with Windows 10 and 11).");
        if (!unpack.output.empty()) {
            sayProgramOutput(unpack.output);
        }
        return Outcome::Failed;
    }

    say(format(L"  Running the Interception installer (%s)...", verb));
    const RunResult run = runProcess(installer, argument, 120000);
    if (!run.started) {
        say(format(L"  Could not start the Interception installer (Windows error %lu).", run.startError));
        return Outcome::Failed;
    }
    sayProgramOutput(run.output);
    if (run.timedOut) {
        say(L"  The Interception installer did not finish in time and was stopped.");
        return Outcome::Failed;
    }
    note(format(L"install-interception.exe exit code %lu", run.exitCode));
    if (run.exitCode != 0) {
        say(format(L"  The Interception installer failed (exit code %lu).", run.exitCode));
        return Outcome::Failed;
    }

    const FgcInterceptionState state = currentState(env);
    if (installing) {
        if (state == FGC_INTERCEPTION_NOT_INSTALLED) {
            say(L"  The installer reported success but the driver is still not present. Check that antivirus did not block it.");
            return Outcome::Failed;
        }
        say(L"  Installed. Restart Windows to activate it.");
    } else {
        if (state != FGC_INTERCEPTION_NOT_INSTALLED) {
            say(L"  The installer reported success but the driver is still present.");
            return Outcome::Failed;
        }
        say(L"  Removed. Restart Windows to finish.");
    }
    return Outcome::RestartRequired;
}

}  // namespace

Outcome installInterception(const Env& env) {
    say(L"Interception (keyboard capture):");
    const FgcInterceptionState state = currentState(env);
    if (state == FGC_INTERCEPTION_INSTALLED) {
        say(L"  Already installed and active.");
        return Outcome::AlreadyDone;
    }
    if (state == FGC_INTERCEPTION_REBOOT_REQUIRED) {
        say(L"  Already installed. Restart Windows to activate it.");
        return Outcome::AlreadyDone;
    }
    if (env.dryRun) {
        say(L"  Would: use Interception.zip next to this tool, or download Interception v1.0.1 from github.com;");
        say(L"  Would: check its SHA-256 checksum, unpack the installer and run it with /install;");
        say(L"  Would: ask you to restart Windows. Nothing was changed (dry run).");
        return Outcome::Done;
    }
    return runReleaseInstaller(env, L"install", L"/install", true);
}

Outcome uninstallInterception(const Env& env) {
    say(L"Interception (keyboard capture):");
    if (currentState(env) == FGC_INTERCEPTION_NOT_INSTALLED) {
        say(L"  Not installed, nothing to remove.");
        return Outcome::AlreadyDone;
    }
    if (env.dryRun) {
        say(L"  Would: use Interception.zip next to this tool, or download Interception v1.0.1 from github.com;");
        say(L"  Would: check its SHA-256 checksum, unpack the installer and run it with /uninstall;");
        say(L"  Would: ask you to restart Windows. Nothing was changed (dry run).");
        return Outcome::Done;
    }
    return runReleaseInstaller(env, L"remove", L"/uninstall", false);
}

}  // namespace setup
