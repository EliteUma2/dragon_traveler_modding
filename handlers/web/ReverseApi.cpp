#include "pch-il2cpp.h"
#include "ReverseApi.h"
#include "WebServer.h"

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <DbgHelp.h>
#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>
#include <iostream>
#include <mutex>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

namespace ReverseApi {

// ─── JSON helpers (same pattern as ReflectionApi) ───────────────────────────

static std::string JsonEsc(const char* s) {
    if (!s) return "null";
    std::string out;
    out.reserve(128);
    out += '"';
    while (*s) {
        switch (*s) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += *s; break;
        }
        s++;
    }
    out += '"';
    return out;
}

static std::string JsonStr(const std::string& s) { return JsonEsc(s.c_str()); }

static WebServer::HttpResponse JsonOk(const std::string& body) {
    return { 200, "application/json; charset=utf-8", body, {} };
}

static WebServer::HttpResponse JsonErr(int status, const std::string& msg) {
    return { status, "application/json; charset=utf-8",
             "{\"error\":" + JsonStr(msg) + "}", {} };
}

static std::string UrlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = 0, lo = 0;
            if (sscanf(s.c_str() + i + 1, "%1x%1x", &hi, &lo) == 2) {
                out += (char)((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        if (s[i] == '+') out += ' ';
        else out += s[i];
    }
    return out;
}

static std::string GetParam(const WebServer::HttpRequest& req, const std::string& key, const std::string& def = "") {
    auto it = req.params.find(key);
    if (it != req.params.end()) return UrlDecode(it->second);
    return def;
}

static std::string HexAddr(uintptr_t addr) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)addr);
    return buf;
}

// ─── SEH-safe memory helpers (no C++ objects — MSVC C2712) ─────────────────

static size_t SafeMemCopy(void* dst, const void* src, size_t size) {
    __try {
        memcpy(dst, src, size);
        return size;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct PatternMatch { uintptr_t addr; };

static int SafePatternScan(const uint8_t* base, size_t scanSize,
                            const uint8_t* patBytes, const char* patWild, size_t patLen,
                            PatternMatch* out, int maxMatches, uintptr_t scanBase) {
    int count = 0;
    __try {
        for (size_t i = 0; i + patLen <= scanSize && count < maxMatches; i++) {
            bool match = true;
            for (size_t j = 0; j < patLen; j++) {
                if (!patWild[j] && base[i + j] != patBytes[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                out[count].addr = scanBase + i;
                count++;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

struct XrefMatch { uintptr_t addr; char type[8]; };

static int SafeXrefScan(const uint8_t* base, size_t scanSize, uintptr_t scanBase,
                         uintptr_t target, XrefMatch* out, int maxMatches) {
    int count = 0;
    __try {
        for (size_t i = 0; i + 5 <= scanSize && count < maxMatches; i++) {
            if (base[i] == 0xE8 || base[i] == 0xE9) {
                int32_t rel = *(int32_t*)(base + i + 1);
                uintptr_t dest = (scanBase + i) + 5 + rel;
                if (dest == target) {
                    out[count].addr = scanBase + i;
                    strcpy_s(out[count].type, base[i] == 0xE8 ? "call" : "jmp");
                    count++;
                }
            }
            if (i + 7 <= scanSize && base[i] == 0x48 &&
                (base[i + 1] == 0x8D || base[i + 1] == 0x8B)) {
                uint8_t modrm = base[i + 2];
                if ((modrm >> 6) == 0 && (modrm & 7) == 5) {
                    int32_t disp = *(int32_t*)(base + i + 3);
                    uintptr_t dest = (scanBase + i) + 7 + disp;
                    if (dest == target) {
                        out[count].addr = scanBase + i;
                        strcpy_s(out[count].type, base[i + 1] == 0x8D ? "lea" : "mov");
                        count++;
                    }
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}

// ─── Module info cache ─────────────────────────────────────────────────────

struct ModuleEntry {
    std::string name;
    std::string path;
    uintptr_t base;
    uintptr_t end;
    DWORD size;
};

static std::vector<ModuleEntry> GetModules() {
    std::vector<ModuleEntry> result;
    HANDLE proc = GetCurrentProcess();
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModules(proc, mods, sizeof(mods), &needed)) return result;
    int count = needed / sizeof(HMODULE);
    for (int i = 0; i < count; i++) {
        ModuleEntry e;
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mods[i], path, MAX_PATH);
        e.path = path;
        const char* slash = strrchr(path, '\\');
        e.name = slash ? (slash + 1) : path;
        MODULEINFO mi = {};
        GetModuleInformation(proc, mods[i], &mi, sizeof(mi));
        e.base = (uintptr_t)mi.lpBaseOfDll;
        e.size = mi.SizeOfImage;
        e.end = e.base + e.size;
        result.push_back(e);
    }
    return result;
}

static ModuleEntry* FindModuleByName(std::vector<ModuleEntry>& modules, const std::string& name) {
    std::string nameLower = name;
    std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::tolower);
    for (auto& m : modules) {
        std::string mLower = m.name;
        std::transform(mLower.begin(), mLower.end(), mLower.begin(), ::tolower);
        if (mLower == nameLower) return &m;
    }
    return nullptr;
}

// ─── Route: GET /api/modules ───────────────────────────────────────────────

static WebServer::HttpResponse HandleModules(const WebServer::HttpRequest& req) {
    auto modules = GetModules();
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < modules.size(); i++) {
        if (i > 0) js << ",";
        auto& m = modules[i];
        js << "{\"name\":" << JsonStr(m.name)
           << ",\"path\":" << JsonStr(m.path)
           << ",\"base\":\"" << HexAddr(m.base) << "\""
           << ",\"end\":\"" << HexAddr(m.end) << "\""
           << ",\"size\":" << m.size << "}";
    }
    js << "]";
    return JsonOk(js.str());
}

// ─── Route: GET /api/module?addr=0x... ─────────────────────────────────────

static WebServer::HttpResponse HandleModule(const WebServer::HttpRequest& req) {
    std::string addrStr = GetParam(req, "addr");
    if (addrStr.empty()) return JsonErr(400, "missing 'addr'");
    uintptr_t addr = 0;
    try { addr = std::stoull(addrStr, nullptr, 16); } catch (...) {
        return JsonErr(400, "invalid address");
    }

    auto modules = GetModules();
    for (auto& m : modules) {
        if (addr >= m.base && addr < m.end) {
            std::ostringstream js;
            js << "{\"name\":" << JsonStr(m.name)
               << ",\"base\":\"" << HexAddr(m.base) << "\""
               << ",\"offset\":\"" << HexAddr(addr - m.base) << "\""
               << ",\"size\":" << m.size << "}";
            return JsonOk(js.str());
        }
    }
    return JsonErr(404, "address not in any module");
}

// ─── PE parsing helpers ────────────────────────────────────────────────────

static IMAGE_NT_HEADERS* GetNtHeaders(uintptr_t base) {
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    return nt;
}

static IMAGE_SECTION_HEADER* FindSection(IMAGE_NT_HEADERS* nt, const char* name) {
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (strncmp((char*)sec[i].Name, name, 8) == 0) return &sec[i];
    }
    return nullptr;
}

// ─── Route: GET /api/exports?module=X ──────────────────────────────────────

static WebServer::HttpResponse HandleExports(const WebServer::HttpRequest& req) {
    std::string modName = GetParam(req, "module");
    if (modName.empty()) return JsonErr(400, "missing 'module'");

    auto modules = GetModules();
    auto* mod = FindModuleByName(modules, modName);
    if (!mod) return JsonErr(404, "module not found");

    auto* nt = GetNtHeaders(mod->base);
    if (!nt) return JsonErr(500, "invalid PE");

    auto& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (expDir.VirtualAddress == 0) return JsonOk("[]");

    auto* exp = (IMAGE_EXPORT_DIRECTORY*)(mod->base + expDir.VirtualAddress);
    auto* names = (DWORD*)(mod->base + exp->AddressOfNames);
    auto* ordinals = (WORD*)(mod->base + exp->AddressOfNameOrdinals);
    auto* funcs = (DWORD*)(mod->base + exp->AddressOfFunctions);

    int limit = 2000;
    std::ostringstream js;
    js << "[";
    for (DWORD i = 0; i < exp->NumberOfNames && (int)i < limit; i++) {
        if (i > 0) js << ",";
        const char* name = (const char*)(mod->base + names[i]);
        WORD ord = ordinals[i];
        DWORD rva = funcs[ord];
        js << "{\"name\":" << JsonEsc(name)
           << ",\"ordinal\":" << (exp->Base + ord)
           << ",\"rva\":\"" << HexAddr(rva) << "\""
           << ",\"va\":\"" << HexAddr(mod->base + rva) << "\"}";
    }
    js << "]";
    return JsonOk(js.str());
}

// ─── Route: GET /api/imports?module=X ──────────────────────────────────────

static WebServer::HttpResponse HandleImports(const WebServer::HttpRequest& req) {
    std::string modName = GetParam(req, "module");
    if (modName.empty()) return JsonErr(400, "missing 'module'");

    auto modules = GetModules();
    auto* mod = FindModuleByName(modules, modName);
    if (!mod) return JsonErr(404, "module not found");

    auto* nt = GetNtHeaders(mod->base);
    if (!nt) return JsonErr(500, "invalid PE");

    auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress == 0) return JsonOk("[]");

    auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(mod->base + impDir.VirtualAddress);

    int limit = 5000;
    int count = 0;
    std::ostringstream js;
    js << "[";
    bool first = true;
    while (imp->Name != 0 && count < limit) {
        const char* dllName = (const char*)(mod->base + imp->Name);
        auto* thunk = (IMAGE_THUNK_DATA*)(mod->base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* iat = (IMAGE_THUNK_DATA*)(mod->base + imp->FirstThunk);

        while (thunk->u1.AddressOfData != 0 && count < limit) {
            if (!first) js << ",";
            first = false;
            if (thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG) {
                js << "{\"module\":" << JsonEsc(dllName)
                   << ",\"ordinal\":" << IMAGE_ORDINAL(thunk->u1.Ordinal)
                   << ",\"iat\":\"" << HexAddr((uintptr_t)&iat->u1.Function) << "\"}";
            } else {
                auto* hint = (IMAGE_IMPORT_BY_NAME*)(mod->base + thunk->u1.AddressOfData);
                js << "{\"module\":" << JsonEsc(dllName)
                   << ",\"name\":" << JsonEsc(hint->Name)
                   << ",\"iat\":\"" << HexAddr((uintptr_t)&iat->u1.Function) << "\"}";
            }
            thunk++;
            iat++;
            count++;
        }
        imp++;
    }
    js << "]";
    return JsonOk(js.str());
}

// ─── Route: GET /api/threads ───────────────────────────────────────────────

typedef NTSTATUS(NTAPI* NtQueryInformationThread_t)(HANDLE, int, PVOID, ULONG, PULONG);
static NtQueryInformationThread_t pNtQueryInformationThread = nullptr;

static WebServer::HttpResponse HandleThreads(const WebServer::HttpRequest& req) {
    if (!pNtQueryInformationThread) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll) pNtQueryInformationThread = (NtQueryInformationThread_t)GetProcAddress(ntdll, "NtQueryInformationThread");
    }

    DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return JsonErr(500, "CreateToolhelp32Snapshot failed");

    auto modules = GetModules();

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    std::ostringstream js;
    js << "[";
    bool first = true;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (!first) js << ",";
            first = false;

            js << "{\"id\":" << te.th32ThreadID;

            HANDLE ht = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (ht) {
                uintptr_t startAddr = 0;
                if (pNtQueryInformationThread) {
                    pNtQueryInformationThread(ht, 9, &startAddr, sizeof(startAddr), nullptr);
                }
                js << ",\"startAddr\":\"" << HexAddr(startAddr) << "\"";

                for (auto& m : modules) {
                    if (startAddr >= m.base && startAddr < m.end) {
                        js << ",\"module\":" << JsonStr(m.name)
                           << ",\"offset\":\"" << HexAddr(startAddr - m.base) << "\"";
                        break;
                    }
                }
                CloseHandle(ht);
            }

            js << ",\"priority\":" << te.tpBasePri << "}";
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    js << "]";
    return JsonOk(js.str());
}

// ─── Route: GET /api/regions ───────────────────────────────────────────────

static const char* ProtectStr(DWORD p) {
    switch (p & 0xFF) {
    case PAGE_NOACCESS:          return "---";
    case PAGE_READONLY:          return "R--";
    case PAGE_READWRITE:         return "RW-";
    case PAGE_WRITECOPY:         return "RWC";
    case PAGE_EXECUTE:           return "--X";
    case PAGE_EXECUTE_READ:      return "R-X";
    case PAGE_EXECUTE_READWRITE: return "RWX";
    case PAGE_EXECUTE_WRITECOPY: return "RWXC";
    default: return "???";
    }
}

static const char* StateStr(DWORD s) {
    switch (s) {
    case MEM_COMMIT:  return "commit";
    case MEM_RESERVE: return "reserve";
    case MEM_FREE:    return "free";
    default: return "unknown";
    }
}

static const char* TypeStr(DWORD t) {
    switch (t) {
    case MEM_IMAGE:   return "image";
    case MEM_MAPPED:  return "mapped";
    case MEM_PRIVATE: return "private";
    default: return "unknown";
    }
}

static WebServer::HttpResponse HandleRegions(const WebServer::HttpRequest& req) {
    std::string addrStr = GetParam(req, "addr", "0");
    std::string sizeStr = GetParam(req, "size", "0");

    uintptr_t start = 0;
    try { start = std::stoull(addrStr, nullptr, 16); } catch (...) {}

    uintptr_t maxAddr = 0;
    if (!sizeStr.empty() && sizeStr != "0") {
        try { maxAddr = start + std::stoull(sizeStr, nullptr, 16); } catch (...) {}
    }
    if (maxAddr == 0) maxAddr = 0x7FFFFFFFFFFF;

    MEMORY_BASIC_INFORMATION mbi;
    std::ostringstream js;
    js << "[";
    bool first = true;
    int count = 0;
    int limit = 5000;
    uintptr_t addr = start;

    while (addr < maxAddr && count < limit) {
        if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State != MEM_FREE) {
            if (!first) js << ",";
            first = false;
            js << "{\"base\":\"" << HexAddr((uintptr_t)mbi.BaseAddress) << "\""
               << ",\"size\":" << mbi.RegionSize
               << ",\"state\":\"" << StateStr(mbi.State) << "\""
               << ",\"protect\":\"" << ProtectStr(mbi.Protect) << "\""
               << ",\"type\":\"" << TypeStr(mbi.Type) << "\"}";
            count++;
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (addr <= (uintptr_t)mbi.BaseAddress) break;
    }
    js << "]";
    return JsonOk(js.str());
}

// ─── Route: GET /api/symbol?addr=0x... ─────────────────────────────────────

static std::once_flag s_symInitFlag;

static WebServer::HttpResponse HandleSymbol(const WebServer::HttpRequest& req) {
    std::string addrStr = GetParam(req, "addr");
    if (addrStr.empty()) return JsonErr(400, "missing 'addr'");

    uintptr_t addr = 0;
    try { addr = std::stoull(addrStr, nullptr, 16); } catch (...) {
        return JsonErr(400, "invalid address");
    }

    HANDLE proc = GetCurrentProcess();
    std::call_once(s_symInitFlag, [&]() {
        SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
        SymInitialize(proc, NULL, TRUE);
    });

    char symBuf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* sym = (SYMBOL_INFO*)symBuf;
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    std::ostringstream js;
    js << "{\"addr\":\"" << HexAddr(addr) << "\"";

    if (SymFromAddr(proc, addr, &displacement, sym)) {
        js << ",\"name\":" << JsonEsc(sym->Name)
           << ",\"displacement\":" << displacement
           << ",\"symAddr\":\"" << HexAddr((uintptr_t)sym->Address) << "\""
           << ",\"size\":" << sym->Size;

        IMAGEHLP_MODULE64 mod = {};
        mod.SizeOfStruct = sizeof(mod);
        if (SymGetModuleInfo64(proc, addr, &mod)) {
            js << ",\"module\":" << JsonEsc(mod.ModuleName);
        }

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        if (SymGetLineFromAddr64(proc, addr, &lineDisp, &line)) {
            js << ",\"file\":" << JsonEsc(line.FileName)
               << ",\"line\":" << line.LineNumber;
        }
    } else {
        js << ",\"error\":\"no symbol found\"";
    }

    js << "}";
    return JsonOk(js.str());
}

// ─── Route: GET /api/strings?addr=0x...&size=N&minlen=4 ───────────────────

static WebServer::HttpResponse HandleStrings(const WebServer::HttpRequest& req) {
    std::string addrStr = GetParam(req, "addr");
    std::string sizeStr = GetParam(req, "size", "4096");
    std::string minStr  = GetParam(req, "minlen", "4");
    if (addrStr.empty()) return JsonErr(400, "missing 'addr'");

    uintptr_t addr = 0;
    size_t size = 4096;
    int minlen = 4;
    try { addr = std::stoull(addrStr, nullptr, 16); } catch (...) { return JsonErr(400, "invalid addr"); }
    try { size = std::stoull(sizeStr, nullptr, 10); } catch (...) {}
    try { minlen = std::stoi(minStr); } catch (...) {}
    if (size > 1024 * 1024) size = 1024 * 1024;
    if (minlen < 2) minlen = 2;

    // Read memory with SEH protection
    std::vector<uint8_t> buf(size);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)addr, buf.data(), size, &bytesRead)) {
        bytesRead = SafeMemCopy(buf.data(), (void*)addr, size);
        if (bytesRead == 0) return JsonErr(400, "cannot read memory at address");
    }
    buf.resize(bytesRead);

    std::ostringstream js;
    js << "[";
    bool first = true;
    int limit = 1000;
    int count = 0;

    // Scan for ASCII strings
    size_t runStart = 0;
    bool inRun = false;
    for (size_t i = 0; i <= buf.size() && count < limit; i++) {
        bool printable = (i < buf.size()) && (buf[i] >= 0x20 && buf[i] < 0x7F);
        if (printable && !inRun) { runStart = i; inRun = true; }
        if (!printable && inRun) {
            int len = (int)(i - runStart);
            if (len >= minlen) {
                if (!first) js << ",";
                first = false;
                std::string s((char*)&buf[runStart], len);
                js << "{\"offset\":" << runStart
                   << ",\"addr\":\"" << HexAddr(addr + runStart) << "\""
                   << ",\"type\":\"ascii\""
                   << ",\"value\":" << JsonStr(s) << "}";
                count++;
            }
            inRun = false;
        }
    }

    // Scan for UTF-16LE strings
    inRun = false;
    for (size_t i = 0; i + 1 <= buf.size() && count < limit; i += 2) {
        uint16_t ch = *(uint16_t*)&buf[i];
        bool isAsciiWide = (ch >= 0x20 && ch < 0x7F);
        if (isAsciiWide && !inRun) { runStart = i; inRun = true; }
        if (!isAsciiWide && inRun) {
            int charLen = (int)((i - runStart) / 2);
            if (charLen >= minlen) {
                if (!first) js << ",";
                first = false;
                std::string s;
                for (size_t j = runStart; j < i; j += 2) s += (char)buf[j];
                js << "{\"offset\":" << runStart
                   << ",\"addr\":\"" << HexAddr(addr + runStart) << "\""
                   << ",\"type\":\"utf16\""
                   << ",\"value\":" << JsonStr(s) << "}";
                count++;
            }
            inRun = false;
        }
    }

    js << "]";
    return JsonOk(js.str());
}

// ─── Route: GET /api/pattern?module=X&sig=48 89 5C 24 ?? ──────────────────

static WebServer::HttpResponse HandlePattern(const WebServer::HttpRequest& req) {
    std::string modName = GetParam(req, "module");
    std::string sig = GetParam(req, "sig");
    if (modName.empty() || sig.empty()) return JsonErr(400, "missing module/sig");

    auto modules = GetModules();
    auto* mod = FindModuleByName(modules, modName);
    if (!mod) return JsonErr(404, "module not found");

    auto* nt = GetNtHeaders(mod->base);
    if (!nt) return JsonErr(500, "invalid PE");

    uintptr_t scanBase = mod->base;
    size_t scanSize = mod->size;
    auto* textSec = FindSection(nt, ".text");
    if (textSec) {
        scanBase = mod->base + textSec->VirtualAddress;
        scanSize = textSec->Misc.VirtualSize;
    }

    // Parse signature into POD arrays
    std::vector<uint8_t> patBytes;
    std::vector<char> patWild;
    std::istringstream ss(sig);
    std::string tok;
    while (ss >> tok) {
        if (tok == "?" || tok == "??") {
            patBytes.push_back(0);
            patWild.push_back(1);
        } else {
            patBytes.push_back((uint8_t)strtoul(tok.c_str(), nullptr, 16));
            patWild.push_back(0);
        }
    }
    if (patBytes.empty()) return JsonErr(400, "empty signature");

    int limit = 100;
    PatternMatch matches[100];
    int found = SafePatternScan((const uint8_t*)scanBase, scanSize,
                                 patBytes.data(), patWild.data(), patBytes.size(),
                                 matches, limit, scanBase);

    std::ostringstream js;
    js << "{\"module\":" << JsonStr(mod->name)
       << ",\"sig\":" << JsonStr(sig)
       << ",\"matches\":[";

    for (int i = 0; i < found; i++) {
        if (i > 0) js << ",";
        js << "{\"addr\":\"" << HexAddr(matches[i].addr) << "\""
           << ",\"offset\":\"" << HexAddr(matches[i].addr - mod->base) << "\"}";
    }

    js << "],\"count\":" << found << "}";
    return JsonOk(js.str());
}

// ─── Route: GET /api/xrefs?addr=0x...&module=X ────────────────────────────

static WebServer::HttpResponse HandleXrefs(const WebServer::HttpRequest& req) {
    std::string addrStr = GetParam(req, "addr");
    std::string modName = GetParam(req, "module", "GameAssembly.dll");
    if (addrStr.empty()) return JsonErr(400, "missing 'addr'");

    uintptr_t target = 0;
    try { target = std::stoull(addrStr, nullptr, 16); } catch (...) {
        return JsonErr(400, "invalid address");
    }

    auto modules = GetModules();
    auto* mod = FindModuleByName(modules, modName);
    if (!mod) return JsonErr(404, "module not found");

    auto* nt = GetNtHeaders(mod->base);
    if (!nt) return JsonErr(500, "invalid PE");

    uintptr_t scanBase = mod->base;
    size_t scanSize = mod->size;
    auto* textSec = FindSection(nt, ".text");
    if (textSec) {
        scanBase = mod->base + textSec->VirtualAddress;
        scanSize = textSec->Misc.VirtualSize;
    }

    int limit = 200;
    XrefMatch xmatches[200];
    int found = SafeXrefScan((const uint8_t*)scanBase, scanSize, scanBase,
                              target, xmatches, limit);

    std::ostringstream js;
    js << "{\"target\":\"" << HexAddr(target) << "\",\"module\":" << JsonStr(mod->name) << ",\"xrefs\":[";

    for (int i = 0; i < found; i++) {
        if (i > 0) js << ",";
        js << "{\"addr\":\"" << HexAddr(xmatches[i].addr) << "\""
           << ",\"offset\":\"" << HexAddr(xmatches[i].addr - mod->base) << "\""
           << ",\"type\":\"" << xmatches[i].type << "\"}";
    }

    js << "],\"count\":" << found << "}";
    return JsonOk(js.str());
}

// ─── Route: GET /api/processinfo ───────────────────────────────────────────

static WebServer::HttpResponse HandleProcessInfo(const WebServer::HttpRequest& req) {
    HANDLE proc = GetCurrentProcess();
    DWORD pid = GetCurrentProcessId();

    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    GetProcessMemoryInfo(proc, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc));

    DWORD handleCount = 0;
    GetProcessHandleCount(proc, &handleCount);

    FILETIME create, exit, kernel, user;
    GetProcessTimes(proc, &create, &exit, &kernel, &user);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t createTime = ((uint64_t)create.dwHighDateTime << 32) | create.dwLowDateTime;
    uint64_t nowTime = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    uint64_t uptimeSec = (nowTime - createTime) / 10000000ULL;

    auto modules = GetModules();

    std::ostringstream js;
    js << "{\"pid\":" << pid
       << ",\"modules\":" << modules.size()
       << ",\"handles\":" << handleCount
       << ",\"uptimeSeconds\":" << uptimeSec
       << ",\"workingSet\":" << pmc.WorkingSetSize
       << ",\"privateBytes\":" << pmc.PrivateUsage
       << ",\"peakWorkingSet\":" << pmc.PeakWorkingSetSize
       << ",\"pageFaults\":" << pmc.PageFaultCount
       << "}";
    return JsonOk(js.str());
}

// ─── Register all routes ───────────────────────────────────────────────────

void RegisterRoutes() {
    WebServer::Route("GET", "/api/modules",     HandleModules);
    WebServer::Route("GET", "/api/module",      HandleModule);
    WebServer::Route("GET", "/api/exports",     HandleExports);
    WebServer::Route("GET", "/api/imports",     HandleImports);
    WebServer::Route("GET", "/api/threads",     HandleThreads);
    WebServer::Route("GET", "/api/regions",     HandleRegions);
    WebServer::Route("GET", "/api/symbol",      HandleSymbol);
    WebServer::Route("GET", "/api/strings",     HandleStrings);
    WebServer::Route("GET", "/api/pattern",     HandlePattern);
    WebServer::Route("GET", "/api/xrefs",       HandleXrefs);
    WebServer::Route("GET", "/api/processinfo", HandleProcessInfo);

    std::cout << "[WebApi] Reverse API routes registered" << std::endl;
}

} // namespace ReverseApi
