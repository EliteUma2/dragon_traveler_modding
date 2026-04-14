
      // Generated C++ file by Il2CppInspectorPro - https://github.com/jadis0x
      // IL2CPP application initializer
      // Modified: IL2CPP API functions resolved via GetProcAddress (version-independent)

      #include "pch-il2cpp.h"

      #include "il2cpp-appdata.h"
      #include "il2cpp-init.h"
      #include "helpers.h"

      // Minimal Win32 forward declarations — avoid <windows.h> to prevent
      // type conflicts with IL2CPP generated types (IServiceProvider, Polygon, etc.)
      extern "C" __declspec(dllimport) void* __stdcall GetProcAddress(void* hModule, const char* lpProcName);
      extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess(void);

      // IL2CPP APIs — function pointer declarations
      #define DO_API(r, n, p) r (*n) p
      #include "il2cpp-api-functions.h"
      #undef DO_API

      // Application-specific functions (declarations — initialized to nullptr)
      #define DO_APP_FUNC(a, r, n, p) r (*n) p = nullptr
      #define DO_APP_FUNC_METHODINFO(a, n) struct MethodInfo ** n = nullptr
      namespace app {
      #include "il2cpp-functions.h"
      }
      #undef DO_APP_FUNC
      #undef DO_APP_FUNC_METHODINFO

      // TypeInfo pointers (declarations — initialized to nullptr)
      #define DO_TYPEDEF(a, n) n ## __Class** n ## __TypeInfo = nullptr
      namespace app {
      #include "il2cpp-types-ptr.h"
      }
      #undef DO_TYPEDEF

      // Set to true after init to indicate whether app-specific offsets were loaded
      bool g_appOffsetsLoaded = false;

      // IL2CPP application initializer
      void init_il2cpp()
      {
      // Get base address of IL2CPP module
      uintptr_t baseAddress = il2cppi_get_base_address();
      void* hGameAssembly = (void*)baseAddress;

      using namespace app;

      // Resolve IL2CPP API functions via GetProcAddress (version-independent)
      // Falls back to hardcoded RVA offsets if export not found
      #define DO_API(r, n, p) do { \
          void* _proc = GetProcAddress(hGameAssembly, #n); \
          if (_proc) { n = (r (*) p)_proc; } \
          else { n = (r (*) p)(baseAddress + n ## _ptr); } \
      } while(0)
      #include "il2cpp-api-functions.h"
      #undef DO_API

      // Try to load app-specific offsets — skip if module is too small (wrong version)
      // The offsets from il2cpp-functions.h are version-specific; if they point outside
      // the module, we leave everything as nullptr and rely on runtime resolution.
      // Read SizeOfImage directly from PE header using raw offsets
      // DOS header: e_lfanew is at offset 0x3C (4 bytes)
      // NT headers: OptionalHeader.SizeOfImage is at offset 0x50 (4 bytes)
      int32_t peOffset = *(int32_t*)(baseAddress + 0x3C);
      uint32_t sizeOfImage = *(uint32_t*)(baseAddress + peOffset + 0x50);
      uintptr_t moduleSize = sizeOfImage;
      {
          uintptr_t moduleEnd = baseAddress + moduleSize;
          // Quick sanity check: test a known offset to see if it's in range
          // Use the first DO_APP_FUNC offset as canary
          bool offsetsValid = true;
          #define DO_APP_FUNC(a, r, n, p) do { \
              if (offsetsValid && (baseAddress + (a)) >= moduleEnd) { offsetsValid = false; } \
          } while(0)
          #define DO_APP_FUNC_METHODINFO(a, n) do { \
              if (offsetsValid && (baseAddress + (a)) >= moduleEnd) { offsetsValid = false; } \
          } while(0)
          // Just check the first few to avoid overhead — if one is out of range, all are suspect
          // We only need to check a sample, but the macro approach checks all. That's fine.
          #include "il2cpp-functions.h"
          #undef DO_APP_FUNC
          #undef DO_APP_FUNC_METHODINFO

          if (offsetsValid) {
              #define DO_APP_FUNC(a, r, n, p) n = (r (*) p)(baseAddress + a)
              #define DO_APP_FUNC_METHODINFO(a, n) n = (struct MethodInfo **)(baseAddress + a)
              #include "il2cpp-functions.h"
              #undef DO_APP_FUNC
              #undef DO_APP_FUNC_METHODINFO

              #define DO_TYPEDEF(a, n) n ## __TypeInfo = (n ## __Class**) (baseAddress + a);
              #include "il2cpp-types-ptr.h"
              #undef DO_TYPEDEF

              g_appOffsetsLoaded = true;
              DebugLog("init_il2cpp: app-specific offsets loaded (module size: %zu)", moduleSize);
          } else {
              DebugLog("init_il2cpp: app-specific offsets SKIPPED (wrong version — offsets out of range)");
              DebugLog("init_il2cpp: Reflection API will work, but direct function calls/hooks are disabled");
          }
      }
      }
    