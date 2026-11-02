#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace setup {
namespace {

HANDLE g_log = INVALID_HANDLE_VALUE;
std::wstring g_logPath;

void writeConsole(const std::wstring& text) {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    DWORD written = 0;
    if (GetConsoleMode(out, &mode)) {
        WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
    } else {
        const std::string utf8 = toUtf8(text);  // redirected to a file or pipe
        WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

void writeLog(const std::wstring& line) {
    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }
    SYSTEMTIME time;
    GetLocalTime(&time);
    const std::wstring stamped = format(L"%04u-%02u-%02u %02u:%02u:%02u  ", time.wYear, time.wMonth, time.wDay,
                                        time.wHour, time.wMinute, time.wSecond) +
                                 line + L"\r\n";
    const std::string utf8 = toUtf8(stamped);
    DWORD written = 0;
    WriteFile(g_log, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}

HANDLE tryOpen(const std::wstring& path) {
    return CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

}  // namespace

std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
    return wide;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string utf8(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::wstring format(const wchar_t* pattern, ...) {
    va_list args;
    va_start(args, pattern);
    va_list copy;
    va_copy(copy, args);
    const int length = _vscwprintf(pattern, copy);
    va_end(copy);
    std::wstring result;
    if (length > 0) {
        std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
        vswprintf_s(buffer.data(), buffer.size(), pattern, args);
        result.assign(buffer.data(), static_cast<size_t>(length));
    }
    va_end(args);
    return result;
}

std::wstring programDirectory() {
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path, length);
    const size_t slash = full.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring(L".") : full.substr(0, slash);
}

void openLog() {
    g_logPath = programDirectory() + L"\\FgcDriverSetup.log";
    g_log = tryOpen(g_logPath);
    if (g_log == INVALID_HANDLE_VALUE) {
        wchar_t temp[MAX_PATH];
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        g_logPath = std::wstring(temp, length) + L"FgcDriverSetup.log";
        g_log = tryOpen(g_logPath);
    }
    if (g_log != INVALID_HANDLE_VALUE) {
        writeLog(L"---- FgcDriverSetup started ----");
    }
}

const std::wstring& logPath() {
    return g_logPath;
}

void say(const std::wstring& text) {
    writeConsole(text + L"\r\n");
    writeLog(text);
}

void note(const std::wstring& text) {
    writeLog(text);
}

}  // namespace setup
