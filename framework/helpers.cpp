// Generated C++ file by Il2CppInspectorPro - https://github.com/jadis0x
   // Helper functions

#include "pch-il2cpp.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <codecvt>
#include "helpers.h"
#include <iostream>
#include <cstdarg>
#include <cstdio>

// Log file location
extern const LPCWSTR LOG_FILE;

// Helper function to get the module base address
uintptr_t il2cppi_get_base_address() {
 return (uintptr_t)GetModuleHandleW(L"GameAssembly.dll");
}

// Helper function to append text to a file
void il2cppi_log_write(std::string text) {
 HANDLE hfile = CreateFileW(LOG_FILE, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

 if (hfile == INVALID_HANDLE_VALUE)
  MessageBoxW(0, L"Could not open log file", 0, 0);

 DWORD written;
 WriteFile(hfile, text.c_str(), (DWORD)text.length(), &written, NULL);
 WriteFile(hfile, "\r\n", 2, &written, NULL);
 CloseHandle(hfile);
}

void LogError(const char* msg, bool showBox)
{
 std::cout << "[ERROR] " << msg << std::endl;
 il2cppi_log_write(msg);
 if (showBox) MessageBoxA(NULL, msg, "Error", MB_OK | MB_ICONERROR);
}

// Debug log file handle - persistent, flushed after every write
static FILE* g_debugLogFile = nullptr;

void DebugLogInit()
{
 if (g_debugLogFile) return;
 g_debugLogFile = _wfopen(L"DebugTrace.txt", L"w");
 if (g_debugLogFile) {
  fprintf(g_debugLogFile, "=== Debug Trace Started ===\n");
  fflush(g_debugLogFile);
 }
}

void DebugLog(const char* fmt, ...)
{
 if (!g_debugLogFile) DebugLogInit();
 if (!g_debugLogFile) return;

 // Timestamp
 SYSTEMTIME st;
 GetLocalTime(&st);
 fprintf(g_debugLogFile, "[%02d:%02d:%02d.%03d] ",
  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

 va_list args;
 va_start(args, fmt);
 vfprintf(g_debugLogFile, fmt, args);
 va_end(args);

 fprintf(g_debugLogFile, "\n");
 fflush(g_debugLogFile);
}

// Helper function to open a new console window and redirect stdout there
void il2cppi_new_console() {
 AllocConsole();
 freopen_s((FILE**)stdout, "CONOUT$", "w", stdout);
}

#if _MSC_VER >= 1920
// Helper function to convert Il2CppString to std::string
std::string il2cppi_to_string(Il2CppString* str) {
 if (!str)
  return std::string{};

 const auto length = str->length;
 if (length <= 0)
  return std::string{};

 const auto* begin = reinterpret_cast<const char16_t*>(str->chars);
 const auto* end = begin + static_cast<size_t>(length);

 return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(begin, end);
}

// Helper function to convert System.String to std::string
std::string il2cppi_to_string(app::String* str) {
 return il2cppi_to_string(reinterpret_cast<Il2CppString*>(str));
}
app::String* convert_to_system_string(const char* str)
{
 Il2CppString* il2cpp_str = il2cpp_string_new(str);

 if (!il2cpp_str) return nullptr;

 return reinterpret_cast<app::String*>(il2cpp_str);
}


#endif