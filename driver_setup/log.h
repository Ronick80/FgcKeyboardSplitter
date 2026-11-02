#pragma once

#include <string>

namespace setup {

// Opens FgcDriverSetup.log next to the program (or in %TEMP% when that folder is not writable).
void openLog();
const std::wstring& logPath();

// Prints a line to the console and writes it, with a timestamp, to the log.
void say(const std::wstring& text);

// Writes only to the log: technical detail that would clutter the console.
void note(const std::wstring& text);

// printf-style formatting into a wide string.
std::wstring format(const wchar_t* pattern, ...);

std::wstring toWide(const std::string& utf8);
std::string toUtf8(const std::wstring& wide);

// Directory that contains this program, without a trailing backslash.
std::wstring programDirectory();

}  // namespace setup
