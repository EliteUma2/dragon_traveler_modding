#include "pch-il2cpp.h"
#include "RC4Hook.h"
#include "WebServer.h"
#include <Windows.h>
#include "detours/detours.h"

#include <mutex>
#include <vector>
#include <string>
#include <sstream>
#include <iostream>
#include <cstdio>

namespace RC4Hook {

// ─── RC4 object layout (IL2CPP) ─────────────────────────────────────────────
// offset 0x00: Il2CppObject header (16 bytes)
// offset 0x10: _state (byte[], pointer to Il2CppArray)
// offset 0x18: _i (int32)
// offset 0x1C: _j (int32)

// Il2CppArray for byte[]:
// offset 0x00-0x0F: Il2CppObject header
// offset 0x10-0x17: bounds (null for 1D)
// offset 0x18-0x1B: max_length (uint32)
// offset 0x20+:     data

struct RC4LogEntry {
    uint64_t timestamp;       // QueryPerformanceCounter
    int      callIndex;       // sequential index
    // Pre-call state
    uint8_t  sboxBefore[256]; // S-box snapshot before Encrypt
    int32_t  iBefore, jBefore;
    // Parameters
    int32_t  start, end;
    int32_t  dataLen;         // array length
    uint8_t  plaintext[128];  // first 128 bytes of data[start..end] BEFORE encryption
    int32_t  plaintextLen;    // min(end-start, 128)
    // Post-call state
    uint8_t  sboxAfter[256];
    int32_t  iAfter, jAfter;
    uint8_t  ciphertext[128]; // first 128 bytes AFTER encryption
};

static constexpr int MAX_LOG = 64;
static RC4LogEntry s_log[MAX_LOG];
static int s_logHead = 0;   // next write position
static int s_logCount = 0;
static int s_callIndex = 0;
static std::mutex s_mutex;

// ─── Original function pointer ──────────────────────────────────────────────
// RC4.Encrypt(this, byte[] data, int start, int end, MethodInfo* method)
// Native signature: void __fastcall (void* this, void* data, int start, int end, void* methodInfo)
typedef void(__fastcall* RC4Encrypt_t)(void* self, void* data, int32_t start, int32_t end, void* methodInfo);
static RC4Encrypt_t oRC4Encrypt = nullptr;

// Address of RC4.Encrypt — set dynamically
static void* s_rc4EncryptAddr = nullptr;

// ─── Helper: read S-box from RC4 object ─────────────────────────────────────
static bool ReadSBox(void* rc4Obj, uint8_t* out256, int32_t* outI, int32_t* outJ) {
    if (!rc4Obj) return false;
    __try {
        uint8_t* obj = (uint8_t*)rc4Obj;
        void* stateArr = *(void**)(obj + 0x10);
        *outI = *(int32_t*)(obj + 0x18);
        *outJ = *(int32_t*)(obj + 0x1C);
        if (!stateArr) return false;
        uint8_t* arrData = (uint8_t*)stateArr + 0x20; // data starts at offset 0x20
        memcpy(out256, arrData, 256);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ─── Helper: read bytes from IL2CPP byte array ─────────────────────────────
static int ReadArrayBytes(void* arr, int start, int count, uint8_t* out) {
    if (!arr) return 0;
    __try {
        uint8_t* base = (uint8_t*)arr;
        int32_t maxLen = *(int32_t*)(base + 0x18);
        uint8_t* data = base + 0x20;
        int avail = maxLen - start;
        if (avail <= 0) return 0;
        int toCopy = (count < avail) ? count : avail;
        memcpy(out, data + start, toCopy);
        return toCopy;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static int GetArrayLength(void* arr) {
    if (!arr) return 0;
    __try {
        return *(int32_t*)((uint8_t*)arr + 0x18);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ─── Detour function ────────────────────────────────────────────────────────
static void __fastcall hkRC4Encrypt(void* self, void* data, int32_t start, int32_t end, void* methodInfo) {
    LARGE_INTEGER ts;
    QueryPerformanceCounter(&ts);

    RC4LogEntry entry = {};
    entry.timestamp = ts.QuadPart;

    // Read pre-call state
    ReadSBox(self, entry.sboxBefore, &entry.iBefore, &entry.jBefore);
    entry.start = start;
    entry.end = end;
    entry.dataLen = GetArrayLength(data);

    // If end < 0, the game uses data.Length
    int actualEnd = (end >= 0) ? end : entry.dataLen;
    int encLen = actualEnd - start;
    entry.plaintextLen = (encLen > 128) ? 128 : ((encLen > 0) ? encLen : 0);
    ReadArrayBytes(data, start, entry.plaintextLen, entry.plaintext);

    // Call original
    oRC4Encrypt(self, data, start, end, methodInfo);

    // Read post-call state
    ReadSBox(self, entry.sboxAfter, &entry.iAfter, &entry.jAfter);
    ReadArrayBytes(data, start, entry.plaintextLen, entry.ciphertext);

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        entry.callIndex = s_callIndex++;
        s_log[s_logHead] = entry;
        s_logHead = (s_logHead + 1) % MAX_LOG;
        if (s_logCount < MAX_LOG) s_logCount++;
    }
}

// ─── Hex helper ─────────────────────────────────────────────────────────────
static std::string ToHex(const uint8_t* data, int len) {
    std::string out;
    out.reserve(len * 2);
    for (int i = 0; i < len; i++) {
        char buf[3];
        snprintf(buf, sizeof(buf), "%02x", data[i]);
        out += buf;
    }
    return out;
}

// ─── Get JSON log ───────────────────────────────────────────────────────────
std::string GetLog() {
    std::lock_guard<std::mutex> lock(s_mutex);

    std::ostringstream js;
    js << "{\"count\":" << s_logCount << ",\"totalCalls\":" << s_callIndex << ",\"entries\":[";

    int start = (s_logCount < MAX_LOG) ? 0 : s_logHead;
    for (int i = 0; i < s_logCount; i++) {
        int idx = (start + i) % MAX_LOG;
        const auto& e = s_log[idx];
        if (i > 0) js << ",";
        js << "{\"call\":" << e.callIndex
           << ",\"ts\":" << e.timestamp
           << ",\"start\":" << e.start
           << ",\"end\":" << e.end
           << ",\"dataLen\":" << e.dataLen
           << ",\"iBefore\":" << e.iBefore
           << ",\"jBefore\":" << e.jBefore
           << ",\"iAfter\":" << e.iAfter
           << ",\"jAfter\":" << e.jAfter
           << ",\"sboxBefore\":\"" << ToHex(e.sboxBefore, 256) << "\""
           << ",\"sboxAfter\":\"" << ToHex(e.sboxAfter, 256) << "\""
           << ",\"plaintext\":\"" << ToHex(e.plaintext, e.plaintextLen) << "\""
           << ",\"ciphertext\":\"" << ToHex(e.ciphertext, e.plaintextLen) << "\""
           << "}";
    }
    js << "]}";
    return js.str();
}

// ─── Install / Uninstall ────────────────────────────────────────────────────
void Install() {
    // Find RC4 class by iterating all loaded assemblies
    Il2CppClass* klass = nullptr;
    size_t asmCount = 0;
    auto** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &asmCount);
    for (size_t i = 0; i < asmCount && !klass; i++) {
        auto* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        klass = il2cpp_class_from_name(image, "GameEngine.Utils", "RC4");
        if (klass) {
            std::cout << "[RC4Hook] Found RC4 in assembly: " << il2cpp_image_get_name(image) << std::endl;
        }
    }

    if (!klass) {
        std::cout << "[RC4Hook] Failed to find GameEngine.Utils.RC4 class in any assembly" << std::endl;
        return;
    }

    auto method = il2cpp_class_get_method_from_name(klass, "Encrypt", 3);
    if (!method) {
        std::cout << "[RC4Hook] Failed to find RC4.Encrypt method" << std::endl;
        return;
    }

    s_rc4EncryptAddr = (void*)method->methodPointer;
    oRC4Encrypt = (RC4Encrypt_t)s_rc4EncryptAddr;

    std::cout << "[RC4Hook] RC4.Encrypt at " << s_rc4EncryptAddr << std::endl;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (DetourAttach(&(PVOID&)oRC4Encrypt, hkRC4Encrypt) != NO_ERROR) {
        std::cout << "[RC4Hook] DetourAttach failed!" << std::endl;
        DetourTransactionAbort();
        return;
    }
    if (DetourTransactionCommit() != NO_ERROR) {
        std::cout << "[RC4Hook] DetourTransactionCommit failed!" << std::endl;
        return;
    }
    std::cout << "[RC4Hook] Hooked RC4.Encrypt successfully" << std::endl;
}

void Uninstall() {
    if (!oRC4Encrypt || !s_rc4EncryptAddr) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)oRC4Encrypt, hkRC4Encrypt);
    DetourTransactionCommit();

    std::cout << "[RC4Hook] Unhooked RC4.Encrypt" << std::endl;
}

// ─── REST endpoint ──────────────────────────────────────────────────────────
void RegisterRoutes() {
    WebServer::Route("GET", "/api/rc4debug", [](const WebServer::HttpRequest& req) -> WebServer::HttpResponse {
        auto param = req.params.find("clear");
        std::string body = GetLog();
        if (param != req.params.end() && param->second == "1") {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_logHead = 0;
            s_logCount = 0;
        }
        return { 200, "application/json; charset=utf-8", body, {} };
    });

    WebServer::Route("POST", "/api/rc4hook", [](const WebServer::HttpRequest& req) -> WebServer::HttpResponse {
        auto param = req.params.find("action");
        std::string action = (param != req.params.end()) ? param->second : "";
        if (action == "install") {
            Install();
            return { 200, "application/json", "{\"status\":\"installed\"}", {} };
        } else if (action == "uninstall") {
            Uninstall();
            return { 200, "application/json", "{\"status\":\"uninstalled\"}", {} };
        } else {
            return { 400, "application/json", "{\"error\":\"use ?action=install or ?action=uninstall\"}", {} };
        }
    });
}

} // namespace RC4Hook
