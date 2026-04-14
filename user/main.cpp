// Generated C++ file by Il2CppInspectorPro - https://github.com/jadis0x

#include "pch-il2cpp.h"
#include "main.h"
#include <Windows.h>
#include <iostream>
#include <cstdio>

#include "il2cpp-appdata.h"
#include "il2cpp-init.h"
#include "helpers.h"
#include "hooks/InitHooks.h"

HMODULE hModule;
HANDLE hUnloadEvent;

// Set the name of your log file here
extern const LPCWSTR LOG_FILE = L"Logs.txt";

void Run(LPVOID lpParam)
{
 hModule = static_cast<HMODULE>(lpParam);

#ifdef _DEBUG
 il2cppi_new_console();
 SetConsoleTitleA("Debug Console");
#endif

 DebugLogInit();
 DebugLog("=== DLL Loaded, starting initialization ===");
 il2cppi_log_write("Initializing...");

 init_il2cpp();

 if (!AttachIl2Cpp()) return;

 il2cppi_log_write("IL2CPP Thread Attached Successfully.");

 DetourInitilization();

 hUnloadEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
 if (!hUnloadEvent) {
  LogError("Unload Event could not be created!");
  return;
 }

 HANDLE hThread = CreateThread(nullptr, 0, UnloadWatcherThread, hUnloadEvent, 0, nullptr);
 if (hThread) {
  CloseHandle(hThread);
 }
 else {
  LogError("Unload Watcher Thread could not be started!");
 }
}

bool AttachIl2Cpp()
{
 Il2CppDomain* domain = il2cpp_domain_get();
 if (!domain) {
  LogError("IL2CPP Domain not found!", true);
  return false;
 }

 Il2CppThread* thread = il2cpp_thread_attach(domain);
 if (!thread) {
  LogError("IL2CPP Thread attach edilemedi!", true);
  return false;
 }
 return true;
}

DWORD WINAPI UnloadWatcherThread(LPVOID lpParam)
{
 HANDLE eventHandle = static_cast<HANDLE>(lpParam);
 if (!eventHandle) return 0;

 if (WaitForSingleObject(eventHandle, INFINITE) == WAIT_OBJECT_0) {
  std::cout << "\n[INFO]  Unload signal received, exiting..." << std::endl;

  DetourUninitialization();

  fclose(stdout);
  FreeConsole();

  if (hUnloadEvent) {
   CloseHandle(hUnloadEvent);
   hUnloadEvent = nullptr;
  }

  FreeLibraryAndExitThread(hModule, 0);
 }
 return 0;
}