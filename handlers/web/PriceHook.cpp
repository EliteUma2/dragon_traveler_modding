#include "pch-il2cpp.h"
#include "PriceHook.h"
#include "WebServer.h"
#include <Windows.h>
#include "detours/detours.h"

#include <mutex>
#include <vector>
#include <string>
#include <map>
#include <sstream>
#include <iostream>
#include <cstdio>

namespace PriceHook {

// ─── Log entry ──────────────────────────────────────────────────────────────
struct LogEntry {
    uint64_t timestamp;
    std::string originalBody;
    std::string modifiedBody;
    bool modified;
};

static constexpr int MAX_LOG = 64;
static LogEntry s_log[MAX_LOG];
static int s_logHead = 0;
static int s_logCount = 0;
static int s_totalCalls = 0;
static std::mutex s_mutex;

static bool s_enabled = true;

// ─── Dynamic field overrides: field_name -> replacement_value ────────────────
static std::map<std::string, std::string> s_overrides;
static std::mutex s_overridesMutex;

// ─── Original function pointer ──────────────────────────────────────────────
typedef void(__fastcall* UploadHandlerRaw_ctor_t)(void* self, void* data, void* methodInfo);
static UploadHandlerRaw_ctor_t oUploadHandlerRaw_ctor = nullptr;
static void* s_hookAddr = nullptr;

// ─── SEH wrappers (no C++ objects allowed) ──────────────────────────────────

static int SehReadArray(void* arr, const uint8_t** outData) {
    *outData = nullptr;
    if (!arr) return 0;
    __try {
        uint8_t* base = (uint8_t*)arr;
        int32_t len = *(int32_t*)(base + 0x18);
        if (len <= 0 || len > 1024 * 1024) return 0;
        *outData = base + 0x20;
        return len;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

static bool SehWriteArray(void* arr, const char* src, int srcLen) {
    if (!arr || !src) return false;
    __try {
        uint8_t* base = (uint8_t*)arr;
        int32_t maxLen = *(int32_t*)(base + 0x18);
        if (srcLen > maxLen) return false;
        memcpy(base + 0x20, src, srcLen);
        *(int32_t*)(base + 0x18) = srcLen;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static std::string ReadArrayAsString(void* arr) {
    const uint8_t* data = nullptr;
    int len = SehReadArray(arr, &data);
    if (len <= 0 || !data) return "";
    return std::string((const char*)data, len);
}

static bool WriteArrayBytes(void* arr, const std::string& newData) {
    return SehWriteArray(arr, newData.c_str(), (int)newData.size());
}

// ─── Replace a JSON field's value (handles both quoted strings and numbers) ─
static std::string ReplaceJsonField(const std::string& body, const std::string& fieldName,
                                     const std::string& newValue, bool& didModify) {
    // Build key: "fieldName"
    std::string key = "\"";
    key += fieldName;
    key += "\"";

    size_t pos = body.find(key);
    if (pos == std::string::npos) return body;

    size_t p = pos + key.size();

    // Skip whitespace
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;

    // Expect ':'
    if (p >= body.size() || body[p] != ':') return body;
    p++;

    // Skip whitespace
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t')) p++;

    // Determine value type: quoted string, number, or object/array
    bool quoted = (p < body.size() && body[p] == '"');

    size_t valStart, valEnd;

    if (quoted) {
        // Quoted string value: find matching close quote (handle escaped quotes)
        // p points at opening quote
        valStart = p + 1; // first char of value (after opening quote)
        size_t q = valStart;
        while (q < body.size()) {
            if (body[q] == '\\') { q += 2; continue; }
            if (body[q] == '"') break;
            q++;
        }
        valEnd = q; // points at closing quote

        // Reconstruct: body[0..valStart) + newValue + body[valEnd..)
        // valStart is after opening ", valEnd is at closing "
        // Result: ..."fieldName":"newValue",...
        std::string result;
        result.reserve(body.size() + newValue.size());
        result.append(body, 0, valStart);              // includes opening quote
        result.append(newValue);                        // new value
        result.append(body, valEnd, std::string::npos); // from closing quote onward
        didModify = true;
        return result;
    } else {
        // Unquoted value (number, bool, null)
        valStart = p;
        while (p < body.size() && body[p] != ',' && body[p] != '}' && body[p] != ']'
               && body[p] != ' ' && body[p] != '\t' && body[p] != '\n' && body[p] != '\r') {
            p++;
        }
        valEnd = p;

        if (valStart == valEnd) return body;

        std::string result;
        result.reserve(body.size() + newValue.size());
        result.append(body, 0, valStart);
        result.append(newValue);
        result.append(body, valEnd, std::string::npos);
        didModify = true;
        return result;
    }
}

// ─── Apply all overrides to a JSON body ─────────────────────────────────────
static std::string ModifyBody(const std::string& body, bool& didModify) {
    didModify = false;
    std::string result = body;

    std::lock_guard<std::mutex> lock(s_overridesMutex);
    for (const auto& kv : s_overrides) {
        bool changed = false;
        result = ReplaceJsonField(result, kv.first, kv.second, changed);
        if (changed) didModify = true;
    }

    return result;
}

// ─── Detour function ────────────────────────────────────────────────────────
static void __fastcall hkUploadHandlerRaw_ctor(void* self, void* data, void* methodInfo) {
    std::string body = ReadArrayAsString(data);
    std::string modified = body;
    bool didModify = false;

    bool hasOverrides;
    {
        std::lock_guard<std::mutex> lock(s_overridesMutex);
        hasOverrides = !s_overrides.empty();
    }

    if (s_enabled && !body.empty() && hasOverrides) {
        modified = ModifyBody(body, didModify);

        if (didModify) {
            WriteArrayBytes(data, modified);
            std::cout << "[PriceHook] Modified upload body ("
                      << body.size() << "B -> " << modified.size() << "B)" << std::endl;
        }
    }

    // Log all UploadHandlerRaw calls
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        auto& entry = s_log[s_logHead];
        LARGE_INTEGER ts;
        QueryPerformanceCounter(&ts);
        entry.timestamp = ts.QuadPart;
        entry.originalBody = body;
        entry.modifiedBody = didModify ? modified : "";
        entry.modified = didModify;
        s_logHead = (s_logHead + 1) % MAX_LOG;
        if (s_logCount < MAX_LOG) s_logCount++;
        s_totalCalls++;
    }

    oUploadHandlerRaw_ctor(self, data, methodInfo);
}

// ─── JSON escape helper ────────────────────────────────────────────────────
static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// ─── Get JSON log + config ─────────────────────────────────────────────────
std::string GetLog() {
    std::ostringstream js;

    {
        std::lock_guard<std::mutex> lock(s_overridesMutex);
        js << "{\"enabled\":" << (s_enabled ? "true" : "false")
           << ",\"overrides\":{";
        bool first = true;
        for (const auto& kv : s_overrides) {
            if (!first) js << ",";
            js << "\"" << JsonEscape(kv.first) << "\":\"" << JsonEscape(kv.second) << "\"";
            first = false;
        }
        js << "}";
    }

    {
        std::lock_guard<std::mutex> lock(s_mutex);
        js << ",\"count\":" << s_logCount
           << ",\"totalCalls\":" << s_totalCalls
           << ",\"entries\":[";

        int start = (s_logCount < MAX_LOG) ? 0 : s_logHead;
        for (int i = 0; i < s_logCount; i++) {
            int idx = (start + i) % MAX_LOG;
            const auto& e = s_log[idx];
            if (i > 0) js << ",";
            js << "{\"ts\":" << e.timestamp
               << ",\"modified\":" << (e.modified ? "true" : "false")
               << ",\"original\":\"" << JsonEscape(e.originalBody) << "\"";
            if (e.modified) {
                js << ",\"replaced\":\"" << JsonEscape(e.modifiedBody) << "\"";
            }
            js << "}";
        }
        js << "]}";
    }

    return js.str();
}

// ─── Install / Uninstall ───────────────────────────────────────────────────
void Install() {
    if (s_hookAddr) {
        std::cout << "[PriceHook] Already installed" << std::endl;
        return;
    }

    Il2CppClass* klass = nullptr;
    size_t asmCount = 0;
    auto** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &asmCount);
    for (size_t i = 0; i < asmCount && !klass; i++) {
        auto* image = il2cpp_assembly_get_image(assemblies[i]);
        if (!image) continue;
        klass = il2cpp_class_from_name(image, "UnityEngine.Networking", "UploadHandlerRaw");
        if (klass) {
            std::cout << "[PriceHook] Found UploadHandlerRaw in: "
                      << il2cpp_image_get_name(image) << std::endl;
        }
    }

    if (!klass) {
        std::cout << "[PriceHook] Failed to find UploadHandlerRaw class" << std::endl;
        return;
    }

    auto method = il2cpp_class_get_method_from_name(klass, ".ctor", 1);
    if (!method) {
        std::cout << "[PriceHook] Failed to find .ctor(byte[]) method" << std::endl;
        return;
    }

    s_hookAddr = (void*)method->methodPointer;
    oUploadHandlerRaw_ctor = (UploadHandlerRaw_ctor_t)s_hookAddr;

    std::cout << "[PriceHook] UploadHandlerRaw::.ctor at " << s_hookAddr << std::endl;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if (DetourAttach(&(PVOID&)oUploadHandlerRaw_ctor, hkUploadHandlerRaw_ctor) != NO_ERROR) {
        std::cout << "[PriceHook] DetourAttach failed!" << std::endl;
        DetourTransactionAbort();
        s_hookAddr = nullptr;
        return;
    }
    if (DetourTransactionCommit() != NO_ERROR) {
        std::cout << "[PriceHook] DetourTransactionCommit failed!" << std::endl;
        s_hookAddr = nullptr;
        return;
    }
    std::cout << "[PriceHook] Hooked UploadHandlerRaw::.ctor successfully" << std::endl;
}

void Uninstall() {
    if (!oUploadHandlerRaw_ctor || !s_hookAddr) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)oUploadHandlerRaw_ctor, hkUploadHandlerRaw_ctor);
    DetourTransactionCommit();

    s_hookAddr = nullptr;
    std::cout << "[PriceHook] Unhooked UploadHandlerRaw::.ctor" << std::endl;
}

// ─── Simple URL-decode helper ──────────────────────────────────────────────
static std::string UrlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = 0, lo = 0;
            char c1 = s[i+1], c2 = s[i+2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            else { out += s[i]; continue; }
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            else { out += s[i]; continue; }
            out += (char)((hi << 4) | lo);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

// ─── REST endpoints ────────────────────────────────────────────────────────
void RegisterRoutes() {
    WebServer::Route("GET", "/api/pricehook", [](const WebServer::HttpRequest& req) -> WebServer::HttpResponse {
        auto param = req.params.find("clear");
        std::string body = GetLog();
        if (param != req.params.end() && param->second == "1") {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_logHead = 0;
            s_logCount = 0;
        }
        return { 200, "application/json; charset=utf-8", body, {} };
    });

    WebServer::Route("POST", "/api/pricehook", [](const WebServer::HttpRequest& req) -> WebServer::HttpResponse {
        auto action = req.params.find("action");
        std::string act = (action != req.params.end()) ? action->second : "";

        if (act == "install") {
            Install();
            return { 200, "application/json", "{\"status\":\"installed\"}", {} };
        }
        if (act == "uninstall") {
            Uninstall();
            return { 200, "application/json", "{\"status\":\"uninstalled\"}", {} };
        }
        if (act == "enable") {
            s_enabled = true;
            return { 200, "application/json", "{\"status\":\"enabled\"}", {} };
        }
        if (act == "disable") {
            s_enabled = false;
            return { 200, "application/json", "{\"status\":\"disabled\"}", {} };
        }

        // Set a field override: ?action=set&field=goods_price&value=6
        if (act == "set") {
            auto pf = req.params.find("field");
            auto pv = req.params.find("value");
            if (pf == req.params.end() || pv == req.params.end()) {
                return { 400, "application/json", "{\"error\":\"need field and value params\"}", {} };
            }
            std::string field = UrlDecode(pf->second);
            std::string value = UrlDecode(pv->second);
            {
                std::lock_guard<std::mutex> lock(s_overridesMutex);
                s_overrides[field] = value;
            }
            std::ostringstream js;
            js << "{\"status\":\"set\",\"field\":\"" << JsonEscape(field)
               << "\",\"value\":\"" << JsonEscape(value) << "\"}";
            return { 200, "application/json", js.str(), {} };
        }

        // Remove a field override: ?action=remove&field=goods_price
        if (act == "remove") {
            auto pf = req.params.find("field");
            if (pf == req.params.end()) {
                return { 400, "application/json", "{\"error\":\"need field param\"}", {} };
            }
            std::string field = UrlDecode(pf->second);
            {
                std::lock_guard<std::mutex> lock(s_overridesMutex);
                s_overrides.erase(field);
            }
            std::ostringstream js;
            js << "{\"status\":\"removed\",\"field\":\"" << JsonEscape(field) << "\"}";
            return { 200, "application/json", js.str(), {} };
        }

        // Clear all overrides: ?action=clear
        if (act == "clear") {
            std::lock_guard<std::mutex> lock(s_overridesMutex);
            s_overrides.clear();
            return { 200, "application/json", "{\"status\":\"cleared\"}", {} };
        }

        // List current overrides: ?action=list
        if (act == "list") {
            return { 200, "application/json; charset=utf-8", GetLog(), {} };
        }

        return { 400, "application/json",
            "{\"error\":\"actions: install|uninstall|enable|disable|set&field=X&value=Y|remove&field=X|clear|list\"}", {} };
    });
}

} // namespace PriceHook
