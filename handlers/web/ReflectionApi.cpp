#include "pch-il2cpp.h"
#include "ReflectionApi.h"
#include "WebServer.h"
#include "Il2CppResolver.h"
#include <string>
#include <sstream>
#include <vector>
#include <map>
#include <mutex>
#include <algorithm>
#include <iostream>

namespace ReflectionApi {

// ─── JSON helpers ───────────────────────────────────────────────────────────

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

// URL-decode
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

// ─── Route: /api/assemblies ─────────────────────────────────────────────────

static WebServer::HttpResponse HandleAssemblies(const WebServer::HttpRequest& req) {
    size_t count = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &count);

    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < count; i++) {
        if (i > 0) js << ",";
        auto img = il2cpp_assembly_get_image(assemblies[i]);
        const char* name = il2cpp_image_get_name(img);
        size_t classCount = il2cpp_image_get_class_count(img);
        js << "{\"name\":" << JsonEsc(name) << ",\"classCount\":" << classCount << "}";
    }
    js << "]";
    return JsonOk(js.str());
}

// ─── Route: /api/classes ────────────────────────────────────────────────────

static WebServer::HttpResponse HandleClasses(const WebServer::HttpRequest& req) {
    std::string ns = GetParam(req, "ns");
    std::string image = GetParam(req, "image");

    size_t asmCount = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &asmCount);

    std::ostringstream js;
    js << "[";
    bool first = true;

    for (size_t a = 0; a < asmCount; a++) {
        auto img = il2cpp_assembly_get_image(assemblies[a]);
        if (!image.empty()) {
            const char* imgName = il2cpp_image_get_name(img);
            if (image != imgName) continue;
        }

        size_t classCount = il2cpp_image_get_class_count(img);
        for (size_t c = 0; c < classCount; c++) {
            Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(img, c);
            if (!klass) continue;
            const char* classNs = il2cpp_class_get_namespace(klass);
            if (!ns.empty() && ns != (classNs ? classNs : "")) continue;

            if (!first) js << ",";
            first = false;
            const char* className = il2cpp_class_get_name(klass);

            // Count fields and methods
            int fieldCount = 0, methodCount = 0;
            void* iter = nullptr;
            while (il2cpp_class_get_fields(klass, &iter)) fieldCount++;
            iter = nullptr;
            while (il2cpp_class_get_methods(klass, &iter)) methodCount++;

            js << "{\"namespace\":" << JsonEsc(classNs)
               << ",\"name\":" << JsonEsc(className)
               << ",\"fieldCount\":" << fieldCount
               << ",\"methodCount\":" << methodCount << "}";
        }
    }
    js << "]";
    return JsonOk(js.str());
}

// ─── Route: /api/class ──────────────────────────────────────────────────────

static std::string GetTypeName(const Il2CppType* type) {
    if (!type) return "???";
    char* name = il2cpp_type_get_name(type);
    if (!name) return "???";
    std::string result(name);
    il2cpp_free(name);
    return result;
}

static WebServer::HttpResponse HandleClass(const WebServer::HttpRequest& req) {
    std::string ns = GetParam(req, "ns");
    std::string name = GetParam(req, "name");
    if (name.empty()) return JsonErr(400, "missing 'name' param");

    Il2CppClass* klass = Resolver::FindClass(ns.c_str(), name.c_str());
    if (!klass) return JsonErr(404, "class not found");

    std::ostringstream js;
    js << "{";

    // Basic info
    js << "\"namespace\":" << JsonEsc(il2cpp_class_get_namespace(klass));
    js << ",\"name\":" << JsonEsc(il2cpp_class_get_name(klass));

    // Parent
    Il2CppClass* parent = il2cpp_class_get_parent(klass);
    if (parent) {
        js << ",\"parent\":" << JsonEsc(il2cpp_class_get_name(parent));
    }

    // Fields
    js << ",\"fields\":[";
    {
        void* iter = nullptr;
        bool first = true;
        FieldInfo* field;
        while ((field = il2cpp_class_get_fields(klass, &iter))) {
            if (!first) js << ",";
            first = false;
            const char* fname = il2cpp_field_get_name(field);
            const Il2CppType* ftype = il2cpp_field_get_type(field);
            int flags = il2cpp_field_get_flags(field);
            size_t offset = il2cpp_field_get_offset(field);
            bool isStatic = (flags & 0x10) != 0; // FIELD_ATTRIBUTE_STATIC
            js << "{\"name\":" << JsonEsc(fname)
               << ",\"type\":" << JsonStr(GetTypeName(ftype))
               << ",\"offset\":" << offset
               << ",\"isStatic\":" << (isStatic ? "true" : "false") << "}";
        }
    }
    js << "]";

    // Methods
    js << ",\"methods\":[";
    {
        void* iter = nullptr;
        bool first = true;
        const MethodInfo* method;
        while ((method = il2cpp_class_get_methods(klass, &iter))) {
            if (!first) js << ",";
            first = false;
            const char* mname = il2cpp_method_get_name(method);
            uint32_t paramCount = il2cpp_method_get_param_count(method);
            const Il2CppType* retType = il2cpp_method_get_return_type(method);
            uint32_t iflags = 0;
            uint32_t flags = il2cpp_method_get_flags(method, &iflags);
            bool isStatic = (flags & 0x10) != 0; // METHOD_ATTRIBUTE_STATIC

            js << "{\"name\":" << JsonEsc(mname)
               << ",\"returnType\":" << JsonStr(GetTypeName(retType))
               << ",\"isStatic\":" << (isStatic ? "true" : "false")
               << ",\"paramCount\":" << paramCount
               << ",\"params\":[";
            for (uint32_t p = 0; p < paramCount; p++) {
                if (p > 0) js << ",";
                const char* pname = il2cpp_method_get_param_name(method, p);
                const Il2CppType* ptype = il2cpp_method_get_param(method, p);
                js << "{\"name\":" << JsonEsc(pname)
                   << ",\"type\":" << JsonStr(GetTypeName(ptype)) << "}";
            }
            js << "]";

            // Address
            if (method->methodPointer) {
                char addrBuf[32];
                snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)method->methodPointer);
                js << ",\"address\":\"" << addrBuf << "\"";
            }
            js << "}";
        }
    }
    js << "]";

    // Properties
    js << ",\"properties\":[";
    {
        void* iter = nullptr;
        bool first = true;
        const PropertyInfo* prop;
        while ((prop = il2cpp_class_get_properties(klass, &iter))) {
            if (!first) js << ",";
            first = false;
            const char* pname = il2cpp_property_get_name((PropertyInfo*)prop);
            js << "{\"name\":" << JsonEsc(pname) << "}";
        }
    }
    js << "]";

    js << "}";
    return JsonOk(js.str());
}

// ─── Route: /api/search ─────────────────────────────────────────────────────

static WebServer::HttpResponse HandleSearch(const WebServer::HttpRequest& req) {
    std::string q = GetParam(req, "q");
    if (q.empty()) return JsonErr(400, "missing 'q' param");

    // Lowercase query for case-insensitive search
    std::string qLower = q;
    for (auto& c : qLower) c = (char)tolower(c);

    size_t asmCount = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &asmCount);

    std::ostringstream js;
    js << "[";
    bool first = true;
    int maxResults = 200;
    int count = 0;

    for (size_t a = 0; a < asmCount && count < maxResults; a++) {
        auto img = il2cpp_assembly_get_image(assemblies[a]);
        size_t classCount = il2cpp_image_get_class_count(img);
        for (size_t c = 0; c < classCount && count < maxResults; c++) {
            Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(img, c);
            if (!klass) continue;
            const char* className = il2cpp_class_get_name(klass);
            const char* classNs = il2cpp_class_get_namespace(klass);
            if (!className) continue;

            // Check class name
            std::string nameLower = className;
            for (auto& ch : nameLower) ch = (char)tolower(ch);
            bool classMatch = nameLower.find(qLower) != std::string::npos;

            if (classMatch) {
                if (!first) js << ",";
                first = false;
                js << "{\"type\":\"class\",\"namespace\":" << JsonEsc(classNs)
                   << ",\"name\":" << JsonEsc(className) << "}";
                count++;
                continue;
            }

            // Check method names
            void* iter = nullptr;
            const MethodInfo* method;
            while ((method = il2cpp_class_get_methods(klass, &iter)) && count < maxResults) {
                const char* mname = il2cpp_method_get_name(method);
                if (!mname) continue;
                std::string mLower = mname;
                for (auto& ch : mLower) ch = (char)tolower(ch);
                if (mLower.find(qLower) != std::string::npos) {
                    if (!first) js << ",";
                    first = false;
                    js << "{\"type\":\"method\",\"namespace\":" << JsonEsc(classNs)
                       << ",\"class\":" << JsonEsc(className)
                       << ",\"name\":" << JsonEsc(mname) << "}";
                    count++;
                }
            }
        }
    }
    js << "]";
    return JsonOk(js.str());
}

// ─── Route: /api/msgdefs ────────────────────────────────────────────────────
// Returns { "1001": "CSLogin", "1002": "SCLogin", ... } for all ~2042 message definitions

static WebServer::HttpResponse HandleMsgDefs(const WebServer::HttpRequest& req) {
    Il2CppClass* klass = Resolver::FindClass("Game.Message", "MsgDef");
    if (!klass) return JsonErr(404, "MsgDef class not found");

    std::ostringstream js;
    js << "{";
    bool first = true;
    void* iter = nullptr;
    FieldInfo* field;
    while ((field = il2cpp_class_get_fields(klass, &iter))) {
        int flags = il2cpp_field_get_flags(field);
        if (!(flags & 0x10)) continue; // skip non-static
        const Il2CppType* ftype = il2cpp_field_get_type(field);
        std::string typeName = GetTypeName(ftype);
        if (typeName.find("Int32") == std::string::npos && typeName.find("int") == std::string::npos) continue;

        int32_t val = 0;
        il2cpp_field_static_get_value(field, &val);
        const char* fname = il2cpp_field_get_name(field);
        if (!fname) continue;

        if (!first) js << ",";
        first = false;
        js << "\"" << val << "\":" << JsonEsc(fname);
    }
    js << "}";
    return JsonOk(js.str());
}

// ─── Proto tag extraction from MergeFrom native code ────────────────────────
// Scans the compiled MergeFrom method for comparison immediates to find actual
// protobuf wire tag numbers. Returns fieldIndex (1-based) → wire field number.
// This works because MergeFrom contains a switch/if-chain on ReadTag() values.

// SEH-protected helpers (must be separate functions with no C++ objects for __try)
#pragma warning(push)
#pragma warning(disable: 4733)

// Follow JMP/JMP-indirect stubs to reach actual code (up to 3 levels)
static const uint8_t* FollowJmpStub(const uint8_t* ptr) {
    __try {
        for (int i = 0; i < 3; i++) {
            if (ptr[0] == 0xE9) {
                // E9 rel32 — relative JMP
                int32_t offset;
                memcpy(&offset, ptr + 1, 4);
                ptr = ptr + 5 + offset;
            } else if (ptr[0] == 0xFF && ptr[1] == 0x25) {
                // FF 25 rel32 — JMP [rip+rel32] (indirect)
                int32_t offset;
                memcpy(&offset, ptr + 2, 4);
                const uint8_t** target = (const uint8_t**)(ptr + 6 + offset);
                ptr = *target;
            } else {
                break;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ptr;
}

static int ScanCodeHexDump(const uint8_t* fnPtr, int maxBytes, char* outBuf, int outBufSize) {
    int written = 0;
    __try {
        for (int i = 0; i < maxBytes && written + 2 < outBufSize; i++) {
            sprintf(outBuf + written, "%02x", fnPtr[i]);
            written += 2;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    outBuf[written] = 0;
    return written;
}
#pragma warning(pop)

// ─── Route: /api/msgschema ─────────────────────────────────────────────────
// Returns proto field schema for a message class: { name, msgId, fields: [{num, name, type, ...}] }

struct ProtoFieldInfo {
    int num;
    std::string name;
    std::string protoType;
    bool optional;
    bool repeated;
    std::string subType;
    bool battleOp;
};

static std::string MapType(const std::string& typeName, bool& repeated, std::string& subType, bool& battleOp, const std::string& fieldName) {
    repeated = false;
    subType = "";
    battleOp = false;

    if (typeName == "System.Int32" || typeName == "Int32") return "int32";
    if (typeName == "System.Int64" || typeName == "Int64") return "int64";
    if (typeName == "System.UInt32" || typeName == "UInt32") return "uint32";
    if (typeName == "System.UInt64" || typeName == "UInt64") return "uint64";
    if (typeName == "System.Boolean" || typeName == "Boolean") return "bool";
    if (typeName == "System.Single" || typeName == "Single") return "float";
    if (typeName == "System.Double" || typeName == "Double") return "double";
    if (typeName == "System.String" || typeName == "String") return "string";
    if (typeName == "System.Byte[]" || typeName == "Byte[]") return "bytes";

    // ByteString (protobuf bytes)
    if (typeName.find("ByteString") != std::string::npos) {
        if (fieldName.find("operates") != std::string::npos) battleOp = true;
        return "bytes";
    }

    // List<T> (repeated)
    if (typeName.find("List") != std::string::npos) {
        repeated = true;
        auto lt = typeName.find('<');
        auto gt = typeName.rfind('>');
        if (lt != std::string::npos && gt != std::string::npos && gt > lt) {
            std::string inner = typeName.substr(lt + 1, gt - lt - 1);
            // Check if inner type is a Game.Message class
            if (inner.find("Game.Message.") != std::string::npos) {
                auto lastDot = inner.rfind('.');
                subType = inner.substr(lastDot + 1);
                Il2CppClass* innerClass = Resolver::FindClass("Game.Message", subType.c_str());
                if (innerClass && il2cpp_class_is_enum(innerClass)) {
                    return "enum";
                }
                return "message";
            }
            if (inner.find("ByteString") != std::string::npos) {
                if (fieldName.find("operates") != std::string::npos) battleOp = true;
                return "bytes";
            }
            if (inner == "System.Int32" || inner == "Int32") return "int32";
            if (inner == "System.Int64" || inner == "Int64") return "int64";
            if (inner == "System.UInt32" || inner == "UInt32") return "uint32";
            if (inner == "System.UInt64" || inner == "UInt64") return "uint64";
            if (inner == "System.Boolean" || inner == "Boolean") return "bool";
            if (inner == "System.Single" || inner == "Single") return "float";
            if (inner == "System.Double" || inner == "Double") return "double";
            if (inner == "System.String" || inner == "String") return "string";
            return inner; // fallback to raw type name
        }
    }

    // Game.Message.* — could be nested message OR enum
    if (typeName.find("Game.Message.") != std::string::npos) {
        auto lastDot = typeName.rfind('.');
        subType = typeName.substr(lastDot + 1);
        // Check if it's actually an enum
        Il2CppClass* refClass = Resolver::FindClass("Game.Message", subType.c_str());
        if (refClass && il2cpp_class_is_enum(refClass)) {
            return "enum";
        }
        return "message";
    }

    return typeName; // fallback
}

static WebServer::HttpResponse HandleMsgSchema(const WebServer::HttpRequest& req) {
    std::string name = GetParam(req, "name");
    if (name.empty()) return JsonErr(400, "missing 'name' param");

    Il2CppClass* klass = Resolver::FindClass("Game.Message", name.c_str());
    if (!klass) return JsonErr(404, "message class not found");

    // Look up msgId from MsgDef
    int32_t msgId = -1;
    Il2CppClass* msgDefClass = Resolver::FindClass("Game.Message", "MsgDef");
    if (msgDefClass) {
        FieldInfo* idField = il2cpp_class_get_field_from_name(msgDefClass, name.c_str());
        if (idField) {
            il2cpp_field_static_get_value(idField, &msgId);
        }
    }

    // Iterate instance fields, apply skip rules (sequential numbering; use browser overrides.json for corrections)
    std::ostringstream js;
    js << "{\"name\":" << JsonStr(name) << ",\"msgId\":" << msgId << ",\"fields\":[";

    bool first = true;
    void* iter = nullptr;
    FieldInfo* field;
    int fieldNum = 1;

    while ((field = il2cpp_class_get_fields(klass, &iter))) {
        const char* fname = il2cpp_field_get_name(field);
        if (!fname) continue;
        int flags = il2cpp_field_get_flags(field);
        if (flags & 0x10) continue; // skip static

        std::string fieldName(fname);

        // Skip hasXxx boolean markers
        if (fieldName.size() > 3 && fieldName.substr(0, 3) == "has" &&
            fieldName[3] >= 'A' && fieldName[3] <= 'Z') continue;

        // Skip memoizedSerializedSize
        if (fieldName == "memoizedSerializedSize") continue;

        // Strip trailing underscore
        std::string cleanName = fieldName;
        if (!cleanName.empty() && cleanName.back() == '_') {
            cleanName.pop_back();
        }

        // Get type info
        const Il2CppType* ftype = il2cpp_field_get_type(field);
        std::string typeName = GetTypeName(ftype);
        bool repeated = false, battleOp = false;
        std::string subType;
        std::string protoType = MapType(typeName, repeated, subType, battleOp, cleanName);

        // Check if hasXxx exists (→ optional)
        std::string hasCheck = "has";
        if (!cleanName.empty()) {
            hasCheck += (char)toupper((unsigned char)cleanName[0]);
            if (cleanName.size() > 1) hasCheck += cleanName.substr(1);
        }
        bool optional = (il2cpp_class_get_field_from_name(klass, hasCheck.c_str()) != nullptr);

        if (!first) js << ",";
        first = false;
        js << "{\"num\":" << fieldNum
           << ",\"name\":" << JsonStr(cleanName)
           << ",\"type\":" << JsonStr(protoType);
        if (optional) js << ",\"optional\":true";
        if (repeated) js << ",\"repeated\":true";
        if (!subType.empty()) js << ",\"subType\":" << JsonStr(subType);
        if (battleOp) js << ",\"battleOp\":true";
        js << "}";

        fieldNum++;
    }

    js << "]}";
    return JsonOk(js.str());
}

// ─── Route: /api/msgdump ────────────────────────────────────────────────────
// Single call: returns ALL msgId→name mappings AND field schemas for every message class.
// Format: { "defs": {"1001":"CSLogin",...}, "schemas": {"CSLogin":{"msgId":1001,"fields":[...]}, ...} }
// Cached after first call.

static std::string s_msgDumpCache;
static std::mutex s_msgDumpMutex;

static void BuildSchemaFields(Il2CppClass* klass, std::ostringstream& js) {
    // Sequential field numbering; browser-side overrides.json corrects mismatches
    js << "[";
    bool first = true;
    void* iter = nullptr;
    FieldInfo* field;
    int fieldNum = 1;

    while ((field = il2cpp_class_get_fields(klass, &iter))) {
        const char* fname = il2cpp_field_get_name(field);
        if (!fname) continue;
        int flags = il2cpp_field_get_flags(field);
        if (flags & 0x10) continue; // skip static

        std::string fieldName(fname);
        if (fieldName.size() > 3 && fieldName.substr(0, 3) == "has" &&
            fieldName[3] >= 'A' && fieldName[3] <= 'Z') continue;
        if (fieldName == "memoizedSerializedSize") continue;

        std::string cleanName = fieldName;
        if (!cleanName.empty() && cleanName.back() == '_') cleanName.pop_back();

        const Il2CppType* ftype = il2cpp_field_get_type(field);
        std::string typeName = GetTypeName(ftype);
        bool repeated = false, battleOp = false;
        std::string subType;
        std::string protoType = MapType(typeName, repeated, subType, battleOp, cleanName);

        std::string hasCheck = "has";
        if (!cleanName.empty()) {
            hasCheck += (char)toupper((unsigned char)cleanName[0]);
            if (cleanName.size() > 1) hasCheck += cleanName.substr(1);
        }
        bool optional = (il2cpp_class_get_field_from_name(klass, hasCheck.c_str()) != nullptr);

        if (!first) js << ",";
        first = false;
        js << "{\"n\":" << fieldNum
           << ",\"name\":" << JsonStr(cleanName)
           << ",\"t\":" << JsonStr(protoType);
        if (optional) js << ",\"o\":1";
        if (repeated) js << ",\"r\":1";
        if (!subType.empty()) js << ",\"sub\":" << JsonStr(subType);
        if (battleOp) js << ",\"bop\":1";
        js << "}";
        fieldNum++;
    }
    js << "]";
}

static WebServer::HttpResponse HandleMsgDump(const WebServer::HttpRequest& req) {
    // Check cache
    {
        std::lock_guard<std::mutex> lk(s_msgDumpMutex);
        if (!s_msgDumpCache.empty()) {
            bool refresh = GetParam(req, "refresh") == "1";
            if (!refresh) return JsonOk(s_msgDumpCache);
            s_msgDumpCache.clear();
        }
    }

    Il2CppClass* msgDefClass = Resolver::FindClass("Game.Message", "MsgDef");
    if (!msgDefClass) return JsonErr(404, "MsgDef class not found");

    // Step 1: collect all msgId→name pairs from MsgDef
    struct MsgEntry { int32_t id; std::string name; };
    std::vector<MsgEntry> entries;

    {
        void* iter = nullptr;
        FieldInfo* field;
        while ((field = il2cpp_class_get_fields(msgDefClass, &iter))) {
            int flags = il2cpp_field_get_flags(field);
            if (!(flags & 0x10)) continue;
            const Il2CppType* ftype = il2cpp_field_get_type(field);
            std::string typeName = GetTypeName(ftype);
            if (typeName.find("Int32") == std::string::npos && typeName.find("int") == std::string::npos) continue;

            int32_t val = 0;
            il2cpp_field_static_get_value(field, &val);
            const char* fname = il2cpp_field_get_name(field);
            if (fname) entries.push_back({ val, std::string(fname) });
        }
    }

    // Build msgId lookup: name→msgId
    std::map<std::string, int32_t> nameToId;
    for (auto& e : entries) nameToId[e.name] = e.id;

    // Step 2: build defs section
    std::ostringstream js;
    js << "{\"defs\":{";
    for (size_t i = 0; i < entries.size(); i++) {
        if (i > 0) js << ",";
        js << "\"" << entries[i].id << "\":" << JsonStr(entries[i].name);
    }
    js << "},\"schemas\":{";

    // Step 3: iterate ALL classes in Game.Message namespace that have
    // memoizedSerializedSize (= proto message class). This includes both
    // top-level messages (CSLogin, SCPong...) AND sub-types (DHorseLamp, FighterMsg...)
    size_t asmCount = 0;
    auto domain = il2cpp_domain_get();
    auto assemblies = il2cpp_domain_get_assemblies(domain, &asmCount);

    bool firstSchema = true;
    int schemaCount = 0;

    for (size_t a = 0; a < asmCount; a++) {
        auto img = il2cpp_assembly_get_image(assemblies[a]);
        size_t classCount = il2cpp_image_get_class_count(img);
        for (size_t c = 0; c < classCount; c++) {
            Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(img, c);
            if (!klass) continue;
            const char* ns = il2cpp_class_get_namespace(klass);
            if (!ns || strcmp(ns, "Game.Message") != 0) continue;
            const char* className = il2cpp_class_get_name(klass);
            if (!className) continue;

            // Only proto message classes (have memoizedSerializedSize field)
            FieldInfo* memo = il2cpp_class_get_field_from_name(klass, "memoizedSerializedSize");
            if (!memo) continue;

            std::string nameStr(className);
            auto idIt = nameToId.find(nameStr);
            int32_t msgId = (idIt != nameToId.end()) ? idIt->second : -1;

            if (!firstSchema) js << ",";
            firstSchema = false;
            js << JsonStr(nameStr) << ":{\"msgId\":" << msgId << ",\"fields\":";
            BuildSchemaFields(klass, js);
            js << "}";
            schemaCount++;
        }
    }
    js << "},\"enums\":{";

    // Step 4: iterate enum classes in Game.Message namespace and dump constants
    bool firstEnum = true;
    int enumCount = 0;

    for (size_t a = 0; a < asmCount; a++) {
        auto img = il2cpp_assembly_get_image(assemblies[a]);
        size_t classCount = il2cpp_image_get_class_count(img);
        for (size_t c = 0; c < classCount; c++) {
            Il2CppClass* klass = (Il2CppClass*)il2cpp_image_get_class(img, c);
            if (!klass) continue;
            const char* ns = il2cpp_class_get_namespace(klass);
            if (!ns || strcmp(ns, "Game.Message") != 0) continue;
            if (!il2cpp_class_is_enum(klass)) continue;
            const char* className = il2cpp_class_get_name(klass);
            if (!className) continue;

            if (!firstEnum) js << ",";
            firstEnum = false;
            js << JsonStr(std::string(className)) << ":{";

            // Dump static fields (enum constants) with their int values
            void* iter = nullptr;
            FieldInfo* field;
            bool firstConst = true;
            while ((field = il2cpp_class_get_fields(klass, &iter))) {
                int flags = il2cpp_field_get_flags(field);
                if (!(flags & 0x10)) continue; // only static
                const char* fname = il2cpp_field_get_name(field);
                if (!fname) continue;
                // Skip the internal "value__" field
                if (strcmp(fname, "value__") == 0) continue;

                int32_t val = 0;
                il2cpp_field_static_get_value(field, &val);

                if (!firstConst) js << ",";
                firstConst = false;
                js << JsonStr(std::string(fname)) << ":" << val;
            }
            js << "}";
            enumCount++;
        }
    }

    js << "}}";

    std::string result = js.str();

    // Cache it
    {
        std::lock_guard<std::mutex> lk(s_msgDumpMutex);
        s_msgDumpCache = result;
    }

    std::cout << "[WebApi] msgdump: " << entries.size() << " defs, "
              << schemaCount << " schemas, " << enumCount << " enums, "
              << result.size() << " bytes" << std::endl;
    return JsonOk(result);
}

// ─── Route: /api/static ─────────────────────────────────────────────────────

static WebServer::HttpResponse HandleStaticField(const WebServer::HttpRequest& req) {
    std::string ns = GetParam(req, "ns");
    std::string name = GetParam(req, "name");
    std::string field = GetParam(req, "field");
    if (name.empty() || field.empty()) return JsonErr(400, "missing params");

    Il2CppClass* klass = Resolver::FindClass(ns.c_str(), name.c_str());
    if (!klass) return JsonErr(404, "class not found");

    FieldInfo* fi = il2cpp_class_get_field_from_name(klass, field.c_str());
    if (!fi) return JsonErr(404, "field not found");

    int flags = il2cpp_field_get_flags(fi);
    if (!(flags & 0x10)) return JsonErr(400, "field is not static");

    const Il2CppType* ftype = il2cpp_field_get_type(fi);
    std::string typeName = GetTypeName(ftype);

    // Read value based on type
    std::ostringstream js;
    js << "{\"field\":" << JsonEsc(field.c_str()) << ",\"type\":" << JsonStr(typeName);

    // Try to read as common types
    if (typeName == "System.Int32" || typeName == "System.UInt32" || typeName == "Int32" || typeName == "int") {
        int32_t val = 0;
        il2cpp_field_static_get_value(fi, &val);
        js << ",\"value\":" << val;
    } else if (typeName == "System.Int64" || typeName == "Int64" || typeName == "long") {
        int64_t val = 0;
        il2cpp_field_static_get_value(fi, &val);
        js << ",\"value\":" << val;
    } else if (typeName == "System.Single" || typeName == "Single" || typeName == "float") {
        float val = 0;
        il2cpp_field_static_get_value(fi, &val);
        js << ",\"value\":" << val;
    } else if (typeName == "System.Boolean" || typeName == "Boolean" || typeName == "bool") {
        bool val = false;
        il2cpp_field_static_get_value(fi, &val);
        js << ",\"value\":" << (val ? "true" : "false");
    } else {
        // Object/reference type — read as pointer
        void* val = nullptr;
        il2cpp_field_static_get_value(fi, &val);
        char addrBuf[32];
        snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)val);
        js << ",\"value\":\"" << addrBuf << "\"";
    }

    js << "}";
    return JsonOk(js.str());
}

// ─── Route: /api/prototags ──────────────────────────────────────────────────
// Debug endpoint: scans MergeFrom native code to extract actual proto wire tags.
// GET /api/prototags?ns=Game.Message&name=SCGuildExplore
// Returns: { class, fieldCount, tags: { "1": 1, "2": 2, "5": 6, ... }, raw: [8,16,...] }

static WebServer::HttpResponse HandleProtoTags(const WebServer::HttpRequest& req) {
    std::string ns = GetParam(req, "ns", "Game.Message");
    std::string name = GetParam(req, "name");
    if (name.empty()) return JsonErr(400, "missing 'name' param");

    Il2CppClass* klass = Resolver::FindClass(ns.c_str(), name.c_str());
    if (!klass) return JsonErr(404, "class not found");

    // List field names with sequential numbering (diagnostic endpoint)
    std::vector<std::string> fieldNames;
    {
        void* iter = nullptr;
        FieldInfo* field;
        while ((field = il2cpp_class_get_fields(klass, &iter))) {
            const char* fname = il2cpp_field_get_name(field);
            if (!fname) continue;
            int flags = il2cpp_field_get_flags(field);
            if (flags & 0x10) continue;
            std::string fn(fname);
            if (fn.size() > 3 && fn.substr(0, 3) == "has" && fn[3] >= 'A' && fn[3] <= 'Z') continue;
            if (fn == "memoizedSerializedSize") continue;
            std::string clean = fn;
            if (!clean.empty() && clean.back() == '_') clean.pop_back();
            fieldNames.push_back(clean);
        }
    }

    std::ostringstream js;
    js << "{\"class\":" << JsonStr(ns + "." + name)
       << ",\"fieldCount\":" << fieldNames.size();

    js << ",\"fields\":[";
    for (int i = 0; i < (int)fieldNames.size(); i++) {
        if (i > 0) js << ",";
        js << "{\"seq\":" << (i + 1) << ",\"name\":" << JsonStr(fieldNames[i]) << "}";
    }
    js << "]";

    // MergeFrom address info
    {
        const MethodInfo* mergeFrom = nullptr;
        void* miter = nullptr;
        while (auto* method = il2cpp_class_get_methods(klass, &miter)) {
            const char* mname = il2cpp_method_get_name(method);
            if (mname && strcmp(mname, "MergeFrom") == 0 && il2cpp_method_get_param_count(method) == 1) {
                mergeFrom = method;
                break;
            }
        }
        if (mergeFrom && mergeFrom->methodPointer) {
            auto raw = (const uint8_t*)mergeFrom->methodPointer;
            auto resolved = FollowJmpStub(raw);
            js << ",\"mergeFromAddr\":\"0x" << std::hex << (uintptr_t)raw << std::dec << "\"";
            if (resolved != raw) {
                js << ",\"resolvedAddr\":\"0x" << std::hex << (uintptr_t)resolved << std::dec << "\"";
                js << ",\"jmpStub\":true";
            }
        }
    }
    js << "}";
    return JsonOk(js.str());
}

// ─── Route: /api/memscan ────────────────────────────────────────────────────
// Scan native method code for immediate values (general-purpose reverse engineering helper).
// GET /api/memscan?ns=Game.Message&name=SCGuildExplore&method=MergeFrom&bytes=4096
// Returns: { method, addr, immediates: [{ offset, value, hex }] }

// Helper struct for memscan results (POD, used with SEH helper)
struct CmpImmResult {
    int offset;
    uint32_t value;
    int instrType; // 0=cmp al, 1=cmp eax, 2=cmp r,imm8, 3=cmp r,imm32
};

#pragma warning(push)
#pragma warning(disable: 4733)
static int ScanCodeForAllCmpImm(const uint8_t* fnPtr, int scanSize, CmpImmResult* out, int maxResults) {
    int count = 0;
    __try {
        for (int i = 0; i < scanSize - 6 && count < maxResults; i++) {
            uint32_t imm = 0;
            int instrType = -1;

            if (fnPtr[i] == 0x3C) { imm = fnPtr[i+1]; instrType = 0; }
            else if (fnPtr[i] == 0x3D) { memcpy(&imm, fnPtr+i+1, 4); instrType = 1; }
            else if (fnPtr[i] == 0x83 && (fnPtr[i+1] & 0x38) == 0x38) { imm = fnPtr[i+2]; instrType = 2; }
            else if (fnPtr[i] == 0x81 && (fnPtr[i+1] & 0x38) == 0x38) { memcpy(&imm, fnPtr+i+2, 4); instrType = 3; }

            if (instrType >= 0 && imm > 0 && imm < 0x100000) {
                out[count].offset = i;
                out[count].value = imm;
                out[count].instrType = instrType;
                count++;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return count;
}
#pragma warning(pop)

static WebServer::HttpResponse HandleMemScan(const WebServer::HttpRequest& req) {
    std::string ns = GetParam(req, "ns", "Game.Message");
    std::string name = GetParam(req, "name");
    std::string methodName = GetParam(req, "method", "MergeFrom");
    int scanBytesRaw = atoi(GetParam(req, "bytes", "4096").c_str());
    if (scanBytesRaw < 64) scanBytesRaw = 64;
    if (scanBytesRaw > 65536) scanBytesRaw = 65536;
    int scanBytes = scanBytesRaw;

    if (name.empty()) return JsonErr(400, "missing 'name' param");

    Il2CppClass* klass = Resolver::FindClass(ns.c_str(), name.c_str());
    if (!klass) return JsonErr(404, "class not found");

    const MethodInfo* method = nullptr;
    void* miter = nullptr;
    while (auto* m = il2cpp_class_get_methods(klass, &miter)) {
        const char* mname = il2cpp_method_get_name(m);
        if (mname && methodName == mname) {
            method = m;
            break;
        }
    }
    if (!method) return JsonErr(404, "method not found");
    auto fnPtrRaw = (const uint8_t*)method->methodPointer;
    if (!fnPtrRaw) return JsonErr(404, "method has no native pointer");

    // Follow JMP stubs (IL2CPP often emits E9 trampolines)
    auto fnPtr = FollowJmpStub(fnPtrRaw);

    // Scan using SEH-safe helper
    CmpImmResult results[4096];
    int resultCount = ScanCodeForAllCmpImm(fnPtr, scanBytes, results, 4096);

    // Hex dump using SEH-safe helper
    int dumpBytes = scanBytes < 128 ? scanBytes : 128;
    char hexDump[65536];
    ScanCodeHexDump(fnPtr, dumpBytes, hexDump, sizeof(hexDump));

    // Build JSON response (safe C++ code, no __try needed)
    static const char* instrNames[] = { "cmp al", "cmp eax", "cmp r,imm8", "cmp r,imm32" };

    std::ostringstream js;
    js << "{\"method\":" << JsonStr(methodName);
    char addrBuf[64];
    snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)fnPtr);
    js << ",\"addr\":\"" << addrBuf << "\"";
    if (fnPtr != fnPtrRaw) {
        snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)fnPtrRaw);
        js << ",\"stubAddr\":\"" << addrBuf << "\"";
        js << ",\"jmpStub\":true";
    }
    js << ",\"scanBytes\":" << scanBytes;

    js << ",\"cmpImm\":[";
    for (int i = 0; i < resultCount; i++) {
        if (i > 0) js << ",";
        js << "{\"off\":" << results[i].offset
           << ",\"val\":" << results[i].value
           << ",\"fn\":" << (results[i].value >> 3)
           << ",\"wt\":" << (results[i].value & 7)
           << ",\"instr\":" << JsonStr(instrNames[results[i].instrType]) << "}";
    }
    js << "]";

    js << ",\"hexDump\":\"" << hexDump << "\"";
    js << "}";
    return JsonOk(js.str());
}

// ─── Route: /api/memdump ─────────────────────────────────────────────────────
// Raw memory dump for external disassembly.
// GET /api/memdump?addr=0x7ffa...&size=4096  → returns hex string
// GET /api/memdump?ns=Game.Message&name=SCGuildExplore&method=MergeFrom&size=4096
// Option: follow=1 to follow JMP stubs (default: 1)

#pragma warning(push)
#pragma warning(disable: 4733)
static int SafeMemRead(const uint8_t* src, uint8_t* dst, int len) {
    int copied = 0;
    __try {
        for (int i = 0; i < len; i++) {
            dst[i] = src[i];
            copied++;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return copied;
}
#pragma warning(pop)

static WebServer::HttpResponse HandleMemDump(const WebServer::HttpRequest& req) {
    std::string addrStr = GetParam(req, "addr");
    std::string ns = GetParam(req, "ns", "Game.Message");
    std::string name = GetParam(req, "name");
    std::string methodName = GetParam(req, "method");
    int size = atoi(GetParam(req, "size", "4096").c_str());
    if (size < 1) size = 1;
    if (size > 65536) size = 65536;
    bool follow = GetParam(req, "follow", "1") != "0";

    const uint8_t* addr = nullptr;

    if (!addrStr.empty()) {
        // Direct address
        unsigned long long a = 0;
        sscanf(addrStr.c_str(), "0x%llx", &a);
        if (a == 0) sscanf(addrStr.c_str(), "%llx", &a);
        addr = (const uint8_t*)(uintptr_t)a;
    } else if (!name.empty() && !methodName.empty()) {
        // Resolve from class + method
        Il2CppClass* klass = Resolver::FindClass(ns.c_str(), name.c_str());
        if (!klass) return JsonErr(404, "class not found");
        void* miter = nullptr;
        while (auto* m = il2cpp_class_get_methods(klass, &miter)) {
            const char* mname = il2cpp_method_get_name(m);
            if (mname && methodName == mname) {
                addr = (const uint8_t*)m->methodPointer;
                break;
            }
        }
        if (!addr) return JsonErr(404, "method not found");
    } else {
        return JsonErr(400, "provide 'addr' or 'name'+'method' params");
    }

    if (!addr) return JsonErr(400, "invalid address");

    const uint8_t* origAddr = addr;
    if (follow) addr = FollowJmpStub(addr);

    // Read memory safely
    std::vector<uint8_t> buf(size);
    int copied = SafeMemRead(addr, buf.data(), size);

    // Build hex string
    std::string hex;
    hex.reserve(copied * 2);
    for (int i = 0; i < copied; i++) {
        char tmp[4];
        sprintf(tmp, "%02x", buf[i]);
        hex += tmp;
    }

    std::ostringstream js;
    char addrBuf[64];
    snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)addr);
    js << "{\"addr\":\"" << addrBuf << "\"";
    if (addr != origAddr) {
        snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)origAddr);
        js << ",\"stubAddr\":\"" << addrBuf << "\"";
    }
    js << ",\"size\":" << copied;
    js << ",\"hex\":\"" << hex << "\"";
    js << "}";
    return JsonOk(js.str());
}

// ─── Route: /api/methodinfo ──────────────────────────────────────────────────
// Dump raw MethodInfo struct for a class method.
// GET /api/methodinfo?ns=Game.Message&name=CSLogin&method=WriteTo
// Returns: MethodInfo pointer address + hex dump of the struct + parsed fields

static WebServer::HttpResponse HandleMethodInfo(const WebServer::HttpRequest& req) {
    std::string ns = GetParam(req, "ns", "Game.Message");
    std::string name = GetParam(req, "name");
    std::string methodName = GetParam(req, "method", "MergeFrom");
    int paramFilter = atoi(GetParam(req, "params", "-1").c_str());
    if (name.empty()) return JsonErr(400, "missing 'name' param");

    Il2CppClass* klass = Resolver::FindClass(ns.c_str(), name.c_str());
    if (!klass) return JsonErr(404, "class not found");

    const MethodInfo* method = nullptr;
    void* miter = nullptr;
    while (auto* m = il2cpp_class_get_methods(klass, &miter)) {
        const char* mname = il2cpp_method_get_name(m);
        if (mname && methodName == mname) {
            if (paramFilter >= 0 && (int)il2cpp_method_get_param_count(m) != paramFilter) continue;
            method = m;
            break;
        }
    }
    if (!method) return JsonErr(404, "method not found");

    // Dump raw MethodInfo struct bytes (256 bytes should cover it)
    uint8_t rawBuf[256] = {};
    int rawCopied = SafeMemRead((const uint8_t*)method, rawBuf, 256);

    std::string rawHex;
    rawHex.reserve(rawCopied * 2);
    for (int i = 0; i < rawCopied; i++) {
        char tmp[4];
        sprintf(tmp, "%02x", rawBuf[i]);
        rawHex += tmp;
    }

    // Read known MethodInfo fields (IL2CPP layout)
    // struct MethodInfo {
    //   Il2CppMethodPointer methodPointer;      // +0x00
    //   Il2CppMethodPointer virtualMethodPointer;// +0x08 (may or may not exist)
    //   InvokerMethod invoker_method;            // +0x08 or +0x10
    //   const char* name;                        // +0x10 or +0x18
    //   Il2CppClass* klass;                      // +0x18 or +0x20
    //   const Il2CppType* return_type;           // +0x20 or +0x28
    //   const Il2CppType* const* parameters;     // +0x28 or +0x30
    //   const Il2CppRGCTXData* rgctx_data;       // varies
    //   const Il2CppGenericMethod* genericMethod; // varies
    // }

    std::ostringstream js;
    char addrBuf[64];

    snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)method);
    js << "{\"methodInfoAddr\":\"" << addrBuf << "\"";

    snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)method->methodPointer);
    js << ",\"methodPointer\":\"" << addrBuf << "\"";

    js << ",\"name\":" << JsonEsc(il2cpp_method_get_name(method));
    js << ",\"paramCount\":" << il2cpp_method_get_param_count(method);
    js << ",\"flags\":\"0x" << std::hex << method->flags << std::dec << "\"";
    js << ",\"iflags\":\"0x" << std::hex << method->iflags << std::dec << "\"";
    js << ",\"slot\":" << method->slot;
    js << ",\"token\":\"0x" << std::hex << method->token << std::dec << "\"";
    js << ",\"is_generic\":" << (method->is_generic ? "true" : "false");
    js << ",\"is_inflated\":" << (method->is_inflated ? "true" : "false");
    js << ",\"wrapper_type\":" << (method->wrapper_type ? "true" : "false");
    js << ",\"parameters_count\":" << (int)method->parameters_count;

    // Class info
    snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)method->klass);
    js << ",\"klassAddr\":\"" << addrBuf << "\"";
    js << ",\"klassName\":" << JsonEsc(il2cpp_class_get_name(method->klass));

    // Invoker method
    snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)method->invoker_method);
    js << ",\"invokerMethod\":\"" << addrBuf << "\"";

    // Il2CppVariant union: rgctx_data or methodMetadataHandle
    snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)method->Il2CppVariant.rgctx_data);
    js << ",\"rgctxData\":\"" << addrBuf << "\"";

    // genericMethod union
    snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)method->genericMethod);
    js << ",\"genericMethod\":\"" << addrBuf << "\"";

    // Raw hex of the struct
    js << ",\"rawHex\":\"" << rawHex << "\"";

    // Dump rgctx_data if it looks like a valid pointer
    auto rgctx = method->Il2CppVariant.rgctx_data;
    if (rgctx && (uintptr_t)rgctx > 0x10000) {
        // rgctx_data is array of Il2CppRGCTXData { void* rgctxDataDummy; MethodInfo*; Il2CppType*; Il2CppClass* }
        // Dump first 32 entries (each 32 bytes)
        uint8_t rgctxBuf[1024] = {};
        int rgctxCopied = SafeMemRead((const uint8_t*)rgctx, rgctxBuf, 1024);

        std::string rgctxHex;
        for (int j = 0; j < rgctxCopied; j++) {
            char tmp[4]; sprintf(tmp, "%02x", rgctxBuf[j]); rgctxHex += tmp;
        }
        js << ",\"rgctxHex\":\"" << rgctxHex << "\"";

        // Parse as array of pointers
        js << ",\"rgctxPtrs\":[";
        for (int i = 0; i < rgctxCopied - 7; i += 8) {
            if (i > 0) js << ",";
            uint64_t val = 0;
            memcpy(&val, rgctxBuf + i, 8);
            snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)val);
            js << "\"" << addrBuf << "\"";
        }
        js << "]";
    }

    js << "}";
    return JsonOk(js.str());
}

// GET /api/vtable?ns=Game.Message&name=CSLogin
// Returns: all vtable entries for the class, with method names and resolved pointers

static WebServer::HttpResponse HandleVtable(const WebServer::HttpRequest& req) {
    std::string ns = GetParam(req, "ns", "Game.Message");
    std::string name = GetParam(req, "name");
    if (name.empty()) return JsonErr(400, "missing 'name' param");

    Il2CppClass* klass = Resolver::FindClass(ns.c_str(), name.c_str());
    if (!klass) return JsonErr(404, "class not found");

    // Force class initialization
    il2cpp_runtime_class_init(klass);

    char addrBuf[64];
    std::ostringstream js;
    js << "{\"class\":" << JsonEsc(name.c_str());

    snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)klass);
    js << ",\"klassAddr\":\"" << addrBuf << "\"";

    // Report compile-time vtable offset for diagnostics
    js << ",\"vtableOffset\":" << offsetof(Il2CppClass, vtable);
    js << ",\"vtable_count\":" << klass->vtable_count;
    js << ",\"classSize\":" << sizeof(Il2CppClass);

    // Instead of reading vtable directly (offset might be wrong),
    // iterate all methods and collect per-method data including slot
    js << ",\"methods\":[";
    bool first = true;
    void* miter = nullptr;
    while (auto* m = il2cpp_class_get_methods(klass, &miter)) {
        if (m->slot == 65535) continue; // skip non-virtual (slot=0xFFFF)
        if (!first) js << ",";
        first = false;

        js << "{\"slot\":" << m->slot;
        js << ",\"name\":" << JsonEsc(il2cpp_method_get_name(m));
        js << ",\"paramCount\":" << il2cpp_method_get_param_count(m);

        snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)m->methodPointer);
        js << ",\"methodPtr\":\"" << addrBuf << "\"";

        auto resolved = FollowJmpStub((const uint8_t*)m->methodPointer);
        if (resolved != (const uint8_t*)m->methodPointer) {
            snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)resolved);
            js << ",\"resolvedPtr\":\"" << addrBuf << "\"";
        }

        if (m->klass && m->klass != klass) {
            js << ",\"declaringClass\":" << JsonEsc(il2cpp_class_get_name(m->klass));
        }

        js << "}";
    }
    js << "]";

    // Also try reading raw vtable memory at compile-time offset
    size_t vtOff = offsetof(Il2CppClass, vtable);
    const uint8_t* vtPtr = ((const uint8_t*)klass) + vtOff;
    uint8_t vtBuf[256] = {};
    int vtRead = SafeMemRead(vtPtr, vtBuf, (std::min)((size_t)256, (size_t)(klass->vtable_count * 16)));
    if (vtRead > 0) {
        std::string vtHex;
        for (int i = 0; i < vtRead; i++) {
            char tmp[4]; sprintf(tmp, "%02x", vtBuf[i]); vtHex += tmp;
        }
        js << ",\"rawVtableHex\":\"" << vtHex << "\"";
    }

    js << "}";
    return JsonOk(js.str());
}

// GET /api/findpe?search=Game.Message
// Scans process memory for PE files containing a search string in metadata

static WebServer::HttpResponse HandleFindPE(const WebServer::HttpRequest& req) {
    std::string search = GetParam(req, "search", "Game.Message");
    int maxResults = atoi(GetParam(req, "max", "5").c_str());
    if (search.empty()) return JsonErr(400, "missing 'search' param");

    std::ostringstream js;
    js << "{\"search\":" << JsonStr(search) << ",\"results\":[";

    MEMORY_BASIC_INFORMATION mbi;
    const uint8_t* addr = nullptr;
    int found = 0;

    while (found < maxResults && VirtualQuery(addr, &mbi, sizeof(mbi))) {
        addr = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;

        // Only scan committed, readable memory
        if (mbi.State != MEM_COMMIT) continue;
        if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) continue;
        if (mbi.Protect & PAGE_GUARD) continue;

        // Skip very large regions (>64MB) to avoid timeouts
        if (mbi.RegionSize > 64 * 1024 * 1024) continue;

        const uint8_t* base = (const uint8_t*)mbi.BaseAddress;
        size_t size = mbi.RegionSize;

        // Scan for MZ header
        for (size_t i = 0; i + 0x200 < size; i++) {
            uint8_t buf[4] = {};
            if (SafeMemRead(base + i, buf, 2) < 2) break;
            if (buf[0] != 0x4D || buf[1] != 0x5A) continue;

            // Check PE signature
            uint8_t peOffBuf[4] = {};
            if (SafeMemRead(base + i + 0x3C, peOffBuf, 4) < 4) continue;
            uint32_t peOff = *(uint32_t*)peOffBuf;
            if (peOff < 0x40 || peOff > 0x400) continue;

            uint8_t peSig[4] = {};
            if (SafeMemRead(base + i + peOff, peSig, 4) < 4) continue;
            if (peSig[0] != 'P' || peSig[1] != 'E' || peSig[2] != 0 || peSig[3] != 0) continue;

            // Valid PE! Search for the target string in the first 4KB
            uint8_t peHead[4096] = {};
            int peRead = SafeMemRead(base + i, peHead, 4096);

            bool hasString = false;
            for (int j = 0; j + (int)search.size() < peRead; j++) {
                if (memcmp(peHead + j, search.c_str(), search.size()) == 0) {
                    hasString = true;
                    break;
                }
            }

            if (!hasString) {
                // Also check further into the file (metadata is usually in the first 512KB)
                uint8_t metaBuf[4096] = {};
                for (size_t scanOff = 4096; scanOff < 512 * 1024 && !hasString; scanOff += 4096) {
                    int read = SafeMemRead(base + i + scanOff, metaBuf, 4096);
                    if (read < (int)search.size()) break;
                    for (int j = 0; j + (int)search.size() < read; j++) {
                        if (memcmp(metaBuf + j, search.c_str(), search.size()) == 0) {
                            hasString = true;
                            break;
                        }
                    }
                }
            }

            // Estimate PE size from optional header (needed for skip logic below)
            uint16_t numSections = 0;
            SafeMemRead(base + i + peOff + 6, (uint8_t*)&numSections, 2);
            uint16_t optSize = 0;
            SafeMemRead(base + i + peOff + 20, (uint8_t*)&optSize, 2);
            uint32_t sizeOfImage = 0;
            if (optSize >= 60) {
                SafeMemRead(base + i + peOff + 24 + 56, (uint8_t*)&sizeOfImage, 4);
            }

            if (hasString) {
                if (found > 0) js << ",";
                char addrBuf[32];
                snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)(base + i));
                js << "{\"addr\":\"" << addrBuf << "\"";
                js << ",\"sections\":" << numSections;
                js << ",\"optHeaderSize\":" << optSize;
                js << ",\"sizeOfImage\":" << sizeOfImage;
                js << "}";
                found++;
            }

            // Skip past this PE
            i += (std::max)((uint32_t)0x1000, sizeOfImage > 0 ? sizeOfImage : (uint32_t)0x1000);
            if (found >= maxResults) break;
        }
    }

    js << "],\"count\":" << found << "}";
    return JsonOk(js.str());
}

// GET /api/dumppe?addr=0x...&size=N
// Dumps raw PE bytes from memory (returns binary, not JSON)

static WebServer::HttpResponse HandleDumpPE(const WebServer::HttpRequest& req) {
    std::string addrStr = GetParam(req, "addr");
    int size = atoi(GetParam(req, "size", "0").c_str());
    if (addrStr.empty()) return JsonErr(400, "missing 'addr' param");

    uintptr_t addr = 0;
    sscanf(addrStr.c_str(), "0x%llx", (unsigned long long*)&addr);
    if (!addr) sscanf(addrStr.c_str(), "%llx", (unsigned long long*)&addr);
    if (!addr) return JsonErr(400, "invalid addr");

    // Auto-detect size from PE header if not specified
    if (size <= 0) {
        uint8_t header[0x200] = {};
        int hRead = SafeMemRead((const uint8_t*)addr, header, 0x200);
        if (hRead < 0x40 || header[0] != 'M' || header[1] != 'Z') return JsonErr(400, "not a PE file");

        uint32_t peOff = *(uint32_t*)(header + 0x3C);
        if (peOff + 24 + 60 > (uint32_t)hRead) return JsonErr(400, "invalid PE header");

        uint16_t numSections = *(uint16_t*)(header + peOff + 6);
        uint16_t optSize = *(uint16_t*)(header + peOff + 20);

        // Calculate actual file size from section headers
        uint32_t maxFileOff = 0;
        int sectStart = peOff + 24 + optSize;
        for (int s = 0; s < numSections && sectStart + 40 <= hRead; s++) {
            uint32_t rawSize = *(uint32_t*)(header + sectStart + 16);
            uint32_t rawOff = *(uint32_t*)(header + sectStart + 20);
            uint32_t end = rawOff + rawSize;
            if (end > maxFileOff) maxFileOff = end;
            sectStart += 40;
        }
        size = (maxFileOff > 0) ? maxFileOff : 65536;
        // Cap at 16MB
        if (size > 16 * 1024 * 1024) size = 16 * 1024 * 1024;
    }

    std::vector<uint8_t> buf(size);
    int copied = SafeMemRead((const uint8_t*)addr, buf.data(), size);
    if (copied <= 0) return JsonErr(500, "failed to read memory");

    std::string body((const char*)buf.data(), copied);
    return { 200, "application/octet-stream", body,
             {{"Content-Disposition", "attachment; filename=\"dump.dll\""}} };
}

// ─── Helpers: method lookup, UTF-16→UTF-8, field reader ─────────────────────

static const MethodInfo* FindMethodByParamType(Il2CppClass* klass, const char* name,
                                                int argc, int paramIdx, const char* typeSubstr) {
    void* iter = nullptr;
    const MethodInfo* m;
    while ((m = il2cpp_class_get_methods(klass, &iter))) {
        if (strcmp(il2cpp_method_get_name(m), name) != 0) continue;
        if ((int)il2cpp_method_get_param_count(m) != argc) continue;
        const Il2CppType* ptype = il2cpp_method_get_param(m, paramIdx);
        char* ptname = il2cpp_type_get_name(ptype);
        if (ptname && strstr(ptname, typeSubstr)) {
            il2cpp_free(ptname);
            return m;
        }
        if (ptname) il2cpp_free(ptname);
    }
    return nullptr;
}

static std::string Il2CppStrToUtf8(Il2CppString* str) {
    if (!str || str->length <= 0) return "";
    std::string out;
    out.reserve(str->length);
    for (int i = 0; i < str->length; i++) {
        uint16_t ch = str->chars[i];
        if (ch < 0x80) out += (char)ch;
        else if (ch < 0x800) {
            out += (char)(0xC0 | (ch >> 6));
            out += (char)(0x80 | (ch & 0x3F));
        } else {
            out += (char)(0xE0 | (ch >> 12));
            out += (char)(0x80 | ((ch >> 6) & 0x3F));
            out += (char)(0x80 | (ch & 0x3F));
        }
    }
    return out;
}

// Forward declaration
static void ReadFieldsJson(Il2CppObject* obj, Il2CppClass* klass, std::ostringstream& js, int depth);

// ─── Snappy decompression ───────────────────────────────────────────────────
static bool SnappyDecompress(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out) {
    if (srcLen < 1) return false;
    // Read uncompressed length (varint)
    size_t pos = 0;
    uint64_t uncompLen = 0;
    int shift = 0;
    while (pos < srcLen) {
        uint8_t b = src[pos++];
        uncompLen |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
        if (shift > 32) return false;
    }
    if (uncompLen > 0x1000000) return false; // 16MB sanity limit

    out.resize((size_t)uncompLen);
    size_t dp = 0;

    while (pos < srcLen && dp < uncompLen) {
        uint8_t tag = src[pos++];
        int type = tag & 3;
        if (type == 0) {
            // Literal
            uint32_t len = (tag >> 2) + 1;
            if (len == 61 && pos < srcLen) { len = src[pos++] + 1; }
            else if (len == 62 && pos + 1 < srcLen) { len = src[pos] | (src[pos + 1] << 8); pos += 2; len += 1; }
            else if (len == 63 && pos + 2 < srcLen) { len = src[pos] | (src[pos + 1] << 8) | (src[pos + 2] << 16); pos += 3; len += 1; }
            else if (len == 64 && pos + 3 < srcLen) { len = src[pos] | (src[pos + 1] << 8) | (src[pos + 2] << 16) | (src[pos + 3] << 24); pos += 4; len += 1; }
            if (pos + len > srcLen || dp + len > uncompLen) return false;
            memcpy(out.data() + dp, src + pos, len);
            pos += len; dp += len;
        } else if (type == 1) {
            // Copy with 1-byte offset
            uint32_t len = ((tag >> 2) & 7) + 4;
            uint32_t off = ((tag >> 5) << 8);
            if (pos >= srcLen) return false;
            off |= src[pos++];
            if (off == 0 || off > dp) return false;
            for (uint32_t i = 0; i < len; i++) out[dp + i] = out[dp - off + i];
            dp += len;
        } else if (type == 2) {
            // Copy with 2-byte offset
            uint32_t len = (tag >> 2) + 1;
            if (pos + 1 >= srcLen) return false;
            uint32_t off = src[pos] | (src[pos + 1] << 8);
            pos += 2;
            if (off == 0 || off > dp) return false;
            for (uint32_t i = 0; i < len; i++) out[dp + i] = out[dp - off + i];
            dp += len;
        } else {
            // Copy with 4-byte offset
            uint32_t len = (tag >> 2) + 1;
            if (pos + 3 >= srcLen) return false;
            uint32_t off = src[pos] | (src[pos + 1] << 8) | (src[pos + 2] << 16) | (src[pos + 3] << 24);
            pos += 4;
            if (off == 0 || off > dp) return false;
            for (uint32_t i = 0; i < len; i++) out[dp + i] = out[dp - off + i];
            dp += len;
        }
    }
    return dp == uncompLen;
}

// Try to decode a ByteString as a sub-message: infer type from parent class + field name,
// try snappy decompress, then MergeFrom into the inferred class.
static bool TryDecodeByteString(const uint8_t* data, size_t len, const std::string& fieldName,
                                 const std::string& parentClassName, std::ostringstream& js, int depth) {
    if (len == 0 || depth > 3) return false;

    // Infer sub-message class name from parent + field
    // e.g. SCGuildExplore + map → GuildExploreMapMsg
    std::string cap = fieldName;
    cap[0] = toupper(cap[0]);

    std::string base = parentClassName;
    // Strip CS/SC prefix
    if (base.size() > 2 && (base.substr(0, 2) == "CS" || base.substr(0, 2) == "SC"))
        base = base.substr(2);

    // Try: base+Cap+Msg, base+Cap, Cap+Msg, Cap
    std::vector<std::string> candidates;
    candidates.push_back(base + cap + "Msg");
    candidates.push_back(base + cap);
    candidates.push_back(cap + "Msg");
    candidates.push_back(cap);

    // Also strip trailing words from base: GuildExploreSwitch → GuildExplore
    for (size_t i = base.size(); i > 2; i--) {
        if (i < base.size() && base[i] >= 'A' && base[i] <= 'Z') {
            std::string shorter = base.substr(0, i);
            candidates.push_back(shorter + cap + "Msg");
            candidates.push_back(shorter + cap);
        }
    }

    Il2CppClass* subClass = nullptr;
    for (auto& name : candidates) {
        subClass = Resolver::FindClass("Game.Message", name.c_str());
        if (subClass) break;
    }
    if (!subClass) return false;

    // Try snappy decompress first, then raw
    std::vector<uint8_t> decompressed;
    const uint8_t* protoData = data;
    size_t protoLen = len;
    bool wasSnappy = SnappyDecompress(data, len, decompressed);
    if (wasSnappy) {
        protoData = decompressed.data();
        protoLen = decompressed.size();
    }

    // Create CodedInputStream
    Il2CppClass* cisClass = Resolver::FindClass("Google.ProtocolBuffers", "CodedInputStream");
    if (!cisClass) return false;
    const MethodInfo* cisCreate = FindMethodByParamType(cisClass, "CreateInstance", 1, 0, "Byte[]");
    if (!cisCreate) return false;

    Il2CppClass* byteClass = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Byte");
    Il2CppArray* byteArr = il2cpp_array_new(byteClass, protoLen);
    memcpy((uint8_t*)byteArr + sizeof(Il2CppArray), protoData, protoLen);
    void* cisArgs[] = { byteArr };
    auto* cis = Resolver::Protection::SafeRuntimeInvoke(cisCreate, nullptr, cisArgs);
    if (!cis) return false;

    // Create message + .ctor()
    Il2CppObject* msg = il2cpp_object_new(subClass);
    if (!msg) return false;
    const MethodInfo* ctor = il2cpp_class_get_method_from_name(subClass, ".ctor", 0);
    if (ctor) Resolver::Protection::SafeRuntimeInvoke(ctor, msg);

    const MethodInfo* mergeFrom = il2cpp_class_get_method_from_name(subClass, "MergeFrom", 1);
    if (!mergeFrom) return false;
    void* mfArgs[] = { cis };
    auto* result = Resolver::Protection::SafeRuntimeInvoke(mergeFrom, msg, mfArgs);

    // Output as nested object with metadata
    const char* subName = il2cpp_class_get_name(subClass);
    js << "{\"_type\":" << JsonStr(subName ? subName : "?");
    if (wasSnappy) js << ",\"_snappy\":true,\"_compressedSize\":" << len << ",\"_decompressedSize\":" << protoLen;
    js << ",\"_fields\":";
    ReadFieldsJson(msg, subClass, js, depth + 1);
    js << "}";
    return true;
}

// Recursively read all instance fields from a protobuf message object as JSON
static void ReadFieldsJson(Il2CppObject* obj, Il2CppClass* klass, std::ostringstream& js, int depth) {
    if (depth > 15) { js << "\"<too deep>\""; return; }
    js << "{";
    bool first = true;
    void* iter = nullptr;
    FieldInfo* field;

    while ((field = il2cpp_class_get_fields(klass, &iter))) {
        const char* fname = il2cpp_field_get_name(field);
        if (!fname) continue;
        if (il2cpp_field_get_flags(field) & 0x10) continue;

        std::string fn(fname);
        if (fn == "memoizedSerializedSize") continue;
        if (fn.find("bitField") == 0) continue;

        // hasXxx booleans
        if (fn.size() > 3 && fn.substr(0, 3) == "has" && fn[3] >= 'A' && fn[3] <= 'Z') {
            bool v = false;
            il2cpp_field_get_value(obj, field, &v);
            if (!first) js << ","; first = false;
            js << JsonStr(fn) << ":" << (v ? "true" : "false");
            continue;
        }

        std::string clean = fn;
        if (!clean.empty() && clean.back() == '_') clean.pop_back();
        if (!first) js << ","; first = false;

        const Il2CppType* ft = il2cpp_field_get_type(field);
        int tid = il2cpp_type_get_type(ft);

        switch (tid) {
        case IL2CPP_TYPE_BOOLEAN: {
            bool v = false; il2cpp_field_get_value(obj, field, &v);
            js << JsonStr(clean) << ":" << (v ? "true" : "false"); break;
        }
        case IL2CPP_TYPE_I4: {
            int32_t v = 0; il2cpp_field_get_value(obj, field, &v);
            js << JsonStr(clean) << ":" << v; break;
        }
        case IL2CPP_TYPE_U4: {
            uint32_t v = 0; il2cpp_field_get_value(obj, field, &v);
            js << JsonStr(clean) << ":" << v; break;
        }
        case IL2CPP_TYPE_I8: {
            int64_t v = 0; il2cpp_field_get_value(obj, field, &v);
            js << JsonStr(clean) << ":" << v; break;
        }
        case IL2CPP_TYPE_U8: {
            uint64_t v = 0; il2cpp_field_get_value(obj, field, &v);
            js << JsonStr(clean) << ":" << v; break;
        }
        case IL2CPP_TYPE_R4: {
            float v = 0; il2cpp_field_get_value(obj, field, &v);
            js << JsonStr(clean) << ":" << v; break;
        }
        case IL2CPP_TYPE_R8: {
            double v = 0; il2cpp_field_get_value(obj, field, &v);
            js << JsonStr(clean) << ":" << v; break;
        }
        case IL2CPP_TYPE_STRING: {
            Il2CppString* str = nullptr; il2cpp_field_get_value(obj, field, &str);
            js << JsonStr(clean) << ":" << JsonEsc(Il2CppStrToUtf8(str).c_str()); break;
        }
        case IL2CPP_TYPE_VALUETYPE: {
            auto* fc = il2cpp_class_from_type(ft);
            if (fc && il2cpp_class_is_enum(fc)) {
                int32_t v = 0; il2cpp_field_get_value(obj, field, &v);
                js << JsonStr(clean) << ":" << v;
            } else {
                js << JsonStr(clean) << ":null";
            }
            break;
        }
        case IL2CPP_TYPE_CLASS: {
            Il2CppObject* sub = nullptr; il2cpp_field_get_value(obj, field, &sub);
            if (sub) {
                auto* sc = il2cpp_object_get_class(sub);
                const char* sn = il2cpp_class_get_name(sc);
                if (il2cpp_class_get_field_from_name(sc, "memoizedSerializedSize")) {
                    js << JsonStr(clean) << ":";
                    ReadFieldsJson(sub, sc, js, depth + 1);
                } else if (sn && strcmp(sn, "ByteString") == 0) {
                    FieldInfo* bf = il2cpp_class_get_field_from_name(sc, "bytes");
                    Il2CppArray* ba = nullptr;
                    if (bf) il2cpp_field_get_value(sub, bf, &ba);
                    if (ba) {
                        auto len = il2cpp_array_length(ba);
                        uint8_t* bd = (uint8_t*)ba + sizeof(Il2CppArray);
                        // Try to decode as sub-message (with snappy auto-detect)
                        const char* parentName = il2cpp_class_get_name(klass);
                        js << JsonStr(clean) << ":";
                        if (len > 0 && parentName &&
                            TryDecodeByteString(bd, len, clean, parentName, js, depth)) {
                            // Successfully decoded as sub-message
                        } else {
                            // Fallback: hex dump
                            js << "\"";
                            for (uint32_t b = 0; b < len && b < 256; b++) {
                                char h[3]; snprintf(h, 3, "%02x", bd[b]); js << h;
                            }
                            if (len > 256) js << "...(" << len << ")";
                            js << "\"";
                        }
                    } else js << JsonStr(clean) << ":\"\"";
                } else {
                    js << JsonStr(clean) << ":\"<" << (sn ? sn : "?") << ">\"";
                }
            } else {
                js << JsonStr(clean) << ":null";
            }
            break;
        }
        case IL2CPP_TYPE_GENERICINST: {
            Il2CppObject* listObj = nullptr; il2cpp_field_get_value(obj, field, &listObj);
            if (listObj) {
                auto* lc = il2cpp_object_get_class(listObj);
                auto* getCount = il2cpp_class_get_method_from_name(lc, "get_Count", 0);
                auto* getItem = il2cpp_class_get_method_from_name(lc, "get_Item", 1);
                int count = 0;
                if (getCount) {
                    auto* cObj = Resolver::Protection::SafeRuntimeInvoke(getCount, listObj);
                    if (cObj) count = *(int*)il2cpp_object_unbox(cObj);
                }
                js << JsonStr(clean) << ":[";
                int maxItems = 500;
                for (int li = 0; li < count && li < maxItems && getItem; li++) {
                    if (li > 0) js << ",";
                    int32_t idx = li;
                    void* itemArgs[] = { &idx };
                    auto* item = Resolver::Protection::SafeRuntimeInvoke(getItem, listObj, itemArgs);
                    if (!item) { js << "null"; continue; }
                    auto* ic = il2cpp_object_get_class(item);
                    if (il2cpp_class_get_field_from_name(ic, "memoizedSerializedSize")) {
                        ReadFieldsJson(item, ic, js, depth + 1);
                    } else if (il2cpp_class_is_valuetype(ic)) {
                        void* data = il2cpp_object_unbox(item);
                        const char* icn = il2cpp_class_get_name(ic);
                        if (strcmp(icn, "Int32") == 0) js << *(int32_t*)data;
                        else if (strcmp(icn, "Int64") == 0) js << *(int64_t*)data;
                        else if (strcmp(icn, "UInt32") == 0) js << *(uint32_t*)data;
                        else if (strcmp(icn, "UInt64") == 0) js << *(uint64_t*)data;
                        else if (strcmp(icn, "Boolean") == 0) js << (*(bool*)data ? "true" : "false");
                        else if (strcmp(icn, "Single") == 0) js << *(float*)data;
                        else if (strcmp(icn, "Double") == 0) js << *(double*)data;
                        else js << "\"<" << icn << ">\"";
                    } else {
                        const char* icn = il2cpp_class_get_name(ic);
                        if (strcmp(icn, "String") == 0)
                            js << JsonEsc(Il2CppStrToUtf8((Il2CppString*)item).c_str());
                        else
                            js << "\"<" << icn << ">\"";
                    }
                }
                if (count > maxItems) js << ",\"...(" << count << " total)\"";
                js << "]";
            } else {
                js << JsonStr(clean) << ":null";
            }
            break;
        }
        default:
            js << JsonStr(clean) << ":null"; break;
        }
    }
    js << "}";
}

// ─── Route: /api/decode ─────────────────────────────────────────────────────
// Deserializes packet data using the game's own MergeFrom.
// GET /api/decode?msgId=5001&data=0a0548656c6c6f
// GET /api/decode?name=CSLogin&data=...

static WebServer::HttpResponse HandleDecode(const WebServer::HttpRequest& req) {
    il2cpp_thread_attach(il2cpp_domain_get());
    std::string msgIdStr = GetParam(req, "msgId");
    std::string className = GetParam(req, "name");
    std::string hexData = GetParam(req, "data");
    if (hexData.empty()) return JsonErr(400, "missing 'data' param");

    // Hex → bytes
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hexData.size(); i += 2) {
        char hex[3] = { hexData[i], hexData[i + 1], 0 };
        bytes.push_back((uint8_t)strtol(hex, nullptr, 16));
    }

    // Resolve message class
    Il2CppClass* msgClass = nullptr;
    if (!className.empty())
        msgClass = Resolver::FindClass("Game.Message", className.c_str());
    if (!msgClass && !msgIdStr.empty()) {
        int msgId = atoi(msgIdStr.c_str());
        Il2CppClass* def = Resolver::FindClass("Game.Message", "MsgDef");
        if (def) {
            void* it = nullptr; FieldInfo* f;
            while ((f = il2cpp_class_get_fields(def, &it))) {
                if (!(il2cpp_field_get_flags(f) & 0x10)) continue;
                int32_t v = 0; il2cpp_field_static_get_value(f, &v);
                if (v == msgId) { msgClass = Resolver::FindClass("Game.Message", il2cpp_field_get_name(f)); break; }
            }
        }
    }
    if (!msgClass) return JsonErr(404, "message class not found");

    // Create CodedInputStream from byte[]
    Il2CppClass* cisClass = Resolver::FindClass("Google.ProtocolBuffers", "CodedInputStream");
    if (!cisClass) return JsonErr(500, "CodedInputStream not found");
    const MethodInfo* cisCreate = FindMethodByParamType(cisClass, "CreateInstance", 1, 0, "Byte[]");
    if (!cisCreate) return JsonErr(500, "CreateInstance(byte[]) not found");

    Il2CppClass* byteClass = il2cpp_class_from_name(il2cpp_get_corlib(), "System", "Byte");
    Il2CppArray* byteArr = il2cpp_array_new(byteClass, bytes.size());
    memcpy((uint8_t*)byteArr + sizeof(Il2CppArray), bytes.data(), bytes.size());
    void* cisArgs[] = { byteArr };
    auto* cis = Resolver::Protection::SafeRuntimeInvoke(cisCreate, nullptr, cisArgs);
    if (!cis) return JsonErr(500, "CreateInstance failed");

    // Create message instance + call .ctor()
    Il2CppObject* msg = il2cpp_object_new(msgClass);
    if (!msg) return JsonErr(500, "il2cpp_object_new failed");
    const MethodInfo* ctor = il2cpp_class_get_method_from_name(msgClass, ".ctor", 0);
    if (ctor) Resolver::Protection::SafeRuntimeInvoke(ctor, msg);

    // Call MergeFrom(ICodedInputStream)
    const MethodInfo* mergeFrom = il2cpp_class_get_method_from_name(msgClass, "MergeFrom", 1);
    if (!mergeFrom) return JsonErr(500, "MergeFrom not found");
    void* mfArgs[] = { cis };
    Resolver::Protection::SafeRuntimeInvoke(mergeFrom, msg, mfArgs);

    // Read fields
    std::ostringstream js;
    js << "{\"class\":" << JsonStr(il2cpp_class_get_name(msgClass))
       << ",\"bytes\":" << bytes.size()
       << ",\"fields\":";
    ReadFieldsJson(msg, msgClass, js, 0);
    js << "}";
    return JsonOk(js.str());
}

// ─── Route: /api/protomap ───────────────────────────────────────────────────
// Extract field_number → field_name mappings by setting one field at a time,
// calling WriteTo, and parsing the protobuf wire format.
// GET /api/protomap?name=CSLogin        — single class
// GET /api/protomap?all=1               — all message classes (bulk)

static WebServer::HttpResponse HandleProtoMap(const WebServer::HttpRequest& req) {
    il2cpp_thread_attach(il2cpp_domain_get());
    std::string className = GetParam(req, "name");
    bool all = GetParam(req, "all") == "1";

    // Resolve shared dependencies
    Il2CppClass* cosClass = Resolver::FindClass("Google.ProtocolBuffers", "CodedOutputStream");
    Il2CppClass* msClass = Resolver::FindClass("System.IO", "MemoryStream");
    if (!cosClass || !msClass) return JsonErr(500, "CodedOutputStream/MemoryStream not found");

    const MethodInfo* cosCreateStream = FindMethodByParamType(cosClass, "CreateInstance", 1, 0, "Stream");
    const MethodInfo* cosFlush = il2cpp_class_get_method_from_name(cosClass, "Flush", 0);
    const MethodInfo* msCtor = il2cpp_class_get_method_from_name(msClass, ".ctor", 0);
    const MethodInfo* msToArray = il2cpp_class_get_method_from_name(msClass, "ToArray", 0);
    if (!cosCreateStream || !cosFlush || !msCtor || !msToArray)
        return JsonErr(500, "missing MemoryStream/CodedOutputStream methods");

    // Collect target classes
    struct Target { Il2CppClass* klass; std::string name; };
    std::vector<Target> targets;

    if (!className.empty()) {
        auto* k = Resolver::FindClass("Game.Message", className.c_str());
        if (!k) return JsonErr(404, "class not found");
        targets.push_back({ k, className });
    } else if (all) {
        size_t asmCount = 0;
        auto** assemblies = il2cpp_domain_get_assemblies(il2cpp_domain_get(), &asmCount);
        for (size_t a = 0; a < asmCount; a++) {
            auto* img = il2cpp_assembly_get_image(assemblies[a]);
            size_t cc = il2cpp_image_get_class_count(img);
            for (size_t c = 0; c < cc; c++) {
                auto* k = (Il2CppClass*)il2cpp_image_get_class(img, c);
                if (!k) continue;
                const char* ns = il2cpp_class_get_namespace(k);
                if (!ns || strcmp(ns, "Game.Message") != 0) continue;
                if (!il2cpp_class_get_field_from_name(k, "memoizedSerializedSize")) continue;
                const char* n = il2cpp_class_get_name(k);
                if (n) targets.push_back({ k, std::string(n) });
            }
        }
    } else {
        return JsonErr(400, "provide 'name' or 'all=1'");
    }

    std::ostringstream js;
    js << "{\"results\":{";
    bool firstClass = true;

    for (auto& tgt : targets) {
        Il2CppClass* klass = tgt.klass;
        const MethodInfo* writeTo = il2cpp_class_get_method_from_name(klass, "WriteTo", 1);
        const MethodInfo* msgCtor = il2cpp_class_get_method_from_name(klass, ".ctor", 0);
        if (!writeTo || !msgCtor) continue;

        // Collect hasXxx / xxx_ field pairs
        struct FieldPair { FieldInfo* hasField; FieldInfo* valField; std::string cleanName; int typeId; };
        std::vector<FieldPair> pairs;
        std::map<std::string, FieldInfo*> hasFields;
        std::vector<std::tuple<FieldInfo*, std::string, int>> valFields;

        void* fiter = nullptr; FieldInfo* fi;
        while ((fi = il2cpp_class_get_fields(klass, &fiter))) {
            if (il2cpp_field_get_flags(fi) & 0x10) continue;
            std::string fn(il2cpp_field_get_name(fi));
            if (fn == "memoizedSerializedSize") continue;
            if (fn.find("bitField") == 0) continue;

            if (fn.size() > 3 && fn.substr(0, 3) == "has" && fn[3] >= 'A' && fn[3] <= 'Z') {
                // Store normalized key: lowercase first char after "has"
                std::string key = fn.substr(3);
                key[0] = (char)tolower((unsigned char)key[0]);
                hasFields[key] = fi;
            } else if (fn.back() == '_' && fn[0] != '<') {
                std::string clean = fn.substr(0, fn.size() - 1);
                const Il2CppType* ft = il2cpp_field_get_type(fi);
                valFields.push_back({ fi, clean, il2cpp_type_get_type(ft) });
            }
        }

        // Match pairs
        for (auto& [vf, clean, tid] : valFields) {
            FieldInfo* hf = nullptr;
            auto hit = hasFields.find(clean);
            if (hit != hasFields.end()) hf = hit->second;
            // Try capitalized version too
            if (!hf && !clean.empty()) {
                std::string capKey = clean;
                capKey[0] = (char)toupper((unsigned char)capKey[0]);
                auto hit2 = hasFields.find(capKey);
                if (hit2 != hasFields.end()) hf = hit2->second;
            }
            pairs.push_back({ hf, vf, clean, tid });
        }

        // For each field: create msg, set has+value, WriteTo, parse tag
        std::vector<std::pair<std::string, int>> mappings; // cleanName → fieldNumber

        for (auto& p : pairs) {
            // Create MemoryStream
            Il2CppObject* ms = il2cpp_object_new(msClass);
            if (!ms) continue;
            Resolver::Protection::SafeRuntimeInvoke(msCtor, ms);

            // Create CodedOutputStream from MemoryStream
            void* cosArgs[] = { ms };
            Il2CppObject* cos = Resolver::Protection::SafeRuntimeInvoke(cosCreateStream, nullptr, cosArgs);
            if (!cos) continue;

            // Create message instance
            Il2CppObject* msg = il2cpp_object_new(klass);
            if (!msg) continue;
            Resolver::Protection::SafeRuntimeInvoke(msgCtor, msg);

            // Set hasXxx = true
            if (p.hasField) {
                bool hasVal = true;
                il2cpp_field_set_value(msg, p.hasField, &hasVal);
            }

            // Set field to non-default value
            bool fieldSet = false;
            switch (p.typeId) {
            case IL2CPP_TYPE_BOOLEAN: { bool v = true; il2cpp_field_set_value(msg, p.valField, &v); fieldSet = true; break; }
            case IL2CPP_TYPE_I4: case IL2CPP_TYPE_U4: { int32_t v = 42; il2cpp_field_set_value(msg, p.valField, &v); fieldSet = true; break; }
            case IL2CPP_TYPE_I8: case IL2CPP_TYPE_U8: { int64_t v = 42; il2cpp_field_set_value(msg, p.valField, &v); fieldSet = true; break; }
            case IL2CPP_TYPE_R4: { float v = 1.5f; il2cpp_field_set_value(msg, p.valField, &v); fieldSet = true; break; }
            case IL2CPP_TYPE_R8: { double v = 1.5; il2cpp_field_set_value(msg, p.valField, &v); fieldSet = true; break; }
            case IL2CPP_TYPE_STRING: {
                Il2CppString* s = il2cpp_string_new("x");
                il2cpp_field_set_value(msg, p.valField, s);
                fieldSet = true; break;
            }
            case IL2CPP_TYPE_VALUETYPE: {
                auto* fc = il2cpp_class_from_type(il2cpp_field_get_type(p.valField));
                if (fc && il2cpp_class_is_enum(fc)) {
                    int32_t v = 1; il2cpp_field_set_value(msg, p.valField, &v); fieldSet = true;
                }
                break;
            }
            case IL2CPP_TYPE_CLASS: {
                auto* fc = il2cpp_class_from_type(il2cpp_field_get_type(p.valField));
                if (fc) {
                    const char* fcn = il2cpp_class_get_name(fc);
                    if (fcn && strcmp(fcn, "ByteString") == 0) {
                        // Create ByteString.Empty or small ByteString
                        FieldInfo* emptyField = il2cpp_class_get_field_from_name(fc, "Empty");
                        if (emptyField) {
                            Il2CppObject* emptyBs = nullptr;
                            il2cpp_field_static_get_value(emptyField, &emptyBs);
                            if (emptyBs) {
                                il2cpp_field_set_value(msg, p.valField, emptyBs);
                                fieldSet = true;
                            }
                        }
                    } else if (il2cpp_class_get_field_from_name(fc, "memoizedSerializedSize")) {
                        // Sub-message: create empty instance
                        Il2CppObject* sub = il2cpp_object_new(fc);
                        if (sub) {
                            auto* subCtor = il2cpp_class_get_method_from_name(fc, ".ctor", 0);
                            if (subCtor) Resolver::Protection::SafeRuntimeInvoke(subCtor, sub);
                            il2cpp_field_set_value(msg, p.valField, sub);
                            fieldSet = true;
                        }
                    }
                }
                break;
            }
            }
            if (!fieldSet) { mappings.push_back({ p.cleanName, -1 }); continue; }

            // WriteTo(cos)
            void* wtArgs[] = { cos };
            Resolver::Protection::SafeRuntimeInvoke(writeTo, msg, wtArgs);
            Resolver::Protection::SafeRuntimeInvoke(cosFlush, cos);

            // MemoryStream.ToArray()
            Il2CppObject* arrObj = Resolver::Protection::SafeRuntimeInvoke(msToArray, ms);
            if (!arrObj) { mappings.push_back({ p.cleanName, -1 }); continue; }

            auto* arr = (Il2CppArray*)arrObj;
            auto arrLen = il2cpp_array_length(arr);
            if (arrLen == 0) { mappings.push_back({ p.cleanName, -1 }); continue; }

            uint8_t* data = (uint8_t*)arr + sizeof(Il2CppArray);

            // Parse first varint tag
            uint32_t tag = 0; int shift = 0;
            for (uint32_t bi = 0; bi < arrLen && bi < 5; bi++) {
                tag |= (uint32_t)(data[bi] & 0x7F) << shift;
                if (!(data[bi] & 0x80)) break;
                shift += 7;
            }
            int fieldNum = tag >> 3;
            mappings.push_back({ p.cleanName, fieldNum });
        }

        // Output
        if (!firstClass) js << ",";
        firstClass = false;
        js << JsonStr(tgt.name) << ":{";
        bool firstField = true;
        for (auto& [name, num] : mappings) {
            if (!firstField) js << ",";
            firstField = false;
            js << JsonStr(name) << ":" << num;
        }
        js << "}";
    }

    js << "},\"count\":" << targets.size() << "}";
    return JsonOk(js.str());
}

// ─── Route: /api/instances ──────────────────────────────────────────────────
// GET /api/instances?ns=Game&name=PlayerMod&limit=50
// FindObjectsOfType → list live instances with addresses and basic field values

static WebServer::HttpResponse HandleInstances(const WebServer::HttpRequest& req) {
    std::string ns = GetParam(req, "ns");
    std::string name = GetParam(req, "name");
    if (name.empty()) return JsonErr(400, "missing 'name' param");
    int limit = 50;
    std::string limitStr = GetParam(req, "limit", "50");
    try { limit = std::stoi(limitStr); } catch (...) {}
    if (limit < 1) limit = 1;
    if (limit > 200) limit = 200;

    Il2CppClass* klass = Resolver::FindClass(ns.c_str(), name.c_str());
    if (!klass) return JsonErr(404, "class not found");

    auto objects = Resolver::FindObjectsByType(klass);

    std::ostringstream js;
    js << "{\"class\":" << JsonEsc(name.c_str()) << ",\"count\":" << objects.size() << ",\"instances\":[";
    int emitted = 0;
    for (auto* obj : objects) {
        if (emitted >= limit) break;
        if (!Resolver::Protection::IsValidIl2CppObject(obj)) continue;
        if (emitted > 0) js << ",";
        char addrBuf[32];
        snprintf(addrBuf, sizeof(addrBuf), "0x%llx", (unsigned long long)(uintptr_t)obj);
        js << "{\"addr\":\"" << addrBuf << "\",\"fields\":";
        ReadFieldsJson(obj, klass, js, 0);
        js << "}";
        emitted++;
    }
    js << "]}";
    return JsonOk(js.str());
}

// ─── Route: /api/readfield ─────────────────────────────────────────────────
// GET /api/readfield?addr=0x1234&field=fieldName

static WebServer::HttpResponse HandleReadField(const WebServer::HttpRequest& req) {
    std::string addrStr = GetParam(req, "addr");
    std::string fieldName = GetParam(req, "field");
    if (addrStr.empty() || fieldName.empty()) return JsonErr(400, "missing 'addr' or 'field'");

    uintptr_t addr = 0;
    try { addr = std::stoull(addrStr, nullptr, 16); } catch (...) {
        return JsonErr(400, "invalid address");
    }
    Il2CppObject* obj = reinterpret_cast<Il2CppObject*>(addr);
    if (!Resolver::Protection::IsValidIl2CppObject(obj)) return JsonErr(400, "invalid object");

    Il2CppClass* klass = il2cpp_object_get_class(obj);
    if (!klass) return JsonErr(500, "cannot get class");

    FieldInfo* fi = il2cpp_class_get_field_from_name(klass, fieldName.c_str());
    if (!fi) return JsonErr(404, "field not found");

    const Il2CppType* ftype = il2cpp_field_get_type(fi);
    std::string typeName = GetTypeName(ftype);

    std::ostringstream js;
    js << "{\"addr\":\"" << addrStr << "\",\"field\":" << JsonStr(fieldName) << ",\"type\":" << JsonStr(typeName);

    if (typeName == "System.Int32" || typeName == "Int32" || typeName == "int") {
        int32_t val = 0; il2cpp_field_get_value(obj, fi, &val);
        js << ",\"value\":" << val;
    } else if (typeName == "System.Int64" || typeName == "Int64" || typeName == "long") {
        int64_t val = 0; il2cpp_field_get_value(obj, fi, &val);
        js << ",\"value\":" << val;
    } else if (typeName == "System.Single" || typeName == "Single" || typeName == "float") {
        float val = 0; il2cpp_field_get_value(obj, fi, &val);
        js << ",\"value\":" << val;
    } else if (typeName == "System.Double" || typeName == "Double" || typeName == "double") {
        double val = 0; il2cpp_field_get_value(obj, fi, &val);
        js << ",\"value\":" << val;
    } else if (typeName == "System.Boolean" || typeName == "Boolean" || typeName == "bool") {
        bool val = false; il2cpp_field_get_value(obj, fi, &val);
        js << ",\"value\":" << (val ? "true" : "false");
    } else if (typeName == "System.String" || typeName == "String") {
        Il2CppString* str = nullptr; il2cpp_field_get_value(obj, fi, &str);
        if (str && str->length >= 0) {
            auto* chars = il2cpp_string_chars(str);
            std::string utf8;
            for (int i = 0; i < str->length; i++) utf8 += (char)chars[i];
            js << ",\"value\":" << JsonStr(utf8);
        } else {
            js << ",\"value\":null";
        }
    } else {
        void* val = nullptr; il2cpp_field_get_value(obj, fi, &val);
        char buf[32]; snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)val);
        js << ",\"value\":\"" << buf << "\"";
    }
    js << "}";
    return JsonOk(js.str());
}

// ─── Minimal JSON parser for POST body fields ──────────────────────────────

static std::string JsonGetString(const std::string& body, const std::string& key) {
    std::string search = "\"" + key + "\"";
    auto pos = body.find(search);
    if (pos == std::string::npos) return "";
    pos = body.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) pos++;
    if (pos >= body.size()) return "";
    if (body[pos] == '"') {
        pos++;
        std::string result;
        while (pos < body.size() && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < body.size()) { result += body[++pos]; }
            else result += body[pos];
            pos++;
        }
        return result;
    }
    // Number or keyword
    std::string result;
    while (pos < body.size() && body[pos] != ',' && body[pos] != '}' && body[pos] != ' ') {
        result += body[pos]; pos++;
    }
    return result;
}

static std::vector<std::string> JsonGetStringArray(const std::string& body, const std::string& key) {
    std::vector<std::string> result;
    std::string search = "\"" + key + "\"";
    auto pos = body.find(search);
    if (pos == std::string::npos) return result;
    pos = body.find('[', pos);
    if (pos == std::string::npos) return result;
    pos++;
    while (pos < body.size() && body[pos] != ']') {
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == ',' || body[pos] == '\t' || body[pos] == '\n')) pos++;
        if (pos >= body.size() || body[pos] == ']') break;
        if (body[pos] == '"') {
            pos++;
            std::string s;
            while (pos < body.size() && body[pos] != '"') {
                if (body[pos] == '\\' && pos + 1 < body.size()) { s += body[++pos]; }
                else s += body[pos];
                pos++;
            }
            if (pos < body.size()) pos++; // skip closing "
            result.push_back(s);
        } else {
            std::string s;
            while (pos < body.size() && body[pos] != ',' && body[pos] != ']') { s += body[pos]; pos++; }
            result.push_back(s);
        }
    }
    return result;
}

// ─── Route: POST /api/writefield ───────────────────────────────────────────
// Body: {"addr":"0x1234","field":"fieldName","value":"123"}

static WebServer::HttpResponse HandleWriteField(const WebServer::HttpRequest& req) {
    std::string addrStr = JsonGetString(req.body, "addr");
    std::string fieldName = JsonGetString(req.body, "field");
    std::string valueStr = JsonGetString(req.body, "value");
    if (addrStr.empty() || fieldName.empty()) return JsonErr(400, "missing addr/field");

    uintptr_t addr = 0;
    try { addr = std::stoull(addrStr, nullptr, 16); } catch (...) {
        return JsonErr(400, "invalid address");
    }
    Il2CppObject* obj = reinterpret_cast<Il2CppObject*>(addr);
    if (!Resolver::Protection::IsValidIl2CppObject(obj)) return JsonErr(400, "invalid object");

    Il2CppClass* klass = il2cpp_object_get_class(obj);
    FieldInfo* fi = il2cpp_class_get_field_from_name(klass, fieldName.c_str());
    if (!fi) return JsonErr(404, "field not found");

    const Il2CppType* ftype = il2cpp_field_get_type(fi);
    std::string typeName = GetTypeName(ftype);

    bool ok = false;
    if (typeName == "System.Int32" || typeName == "Int32" || typeName == "int") {
        int32_t val = std::stoi(valueStr);
        il2cpp_field_set_value(obj, fi, &val); ok = true;
    } else if (typeName == "System.Int64" || typeName == "Int64" || typeName == "long") {
        int64_t val = std::stoll(valueStr);
        il2cpp_field_set_value(obj, fi, &val); ok = true;
    } else if (typeName == "System.Single" || typeName == "Single" || typeName == "float") {
        float val = std::stof(valueStr);
        il2cpp_field_set_value(obj, fi, &val); ok = true;
    } else if (typeName == "System.Double" || typeName == "Double" || typeName == "double") {
        double val = std::stod(valueStr);
        il2cpp_field_set_value(obj, fi, &val); ok = true;
    } else if (typeName == "System.Boolean" || typeName == "Boolean" || typeName == "bool") {
        bool val = (valueStr == "true" || valueStr == "1");
        il2cpp_field_set_value(obj, fi, &val); ok = true;
    } else {
        return JsonErr(400, "unsupported type for write: " + typeName);
    }

    return JsonOk("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
}

// ─── Route: POST /api/writestatic ──────────────────────────────────────────
// Body: {"ns":"Game","name":"SomeClass","field":"fieldName","value":"123"}

static WebServer::HttpResponse HandleWriteStatic(const WebServer::HttpRequest& req) {
    std::string ns = JsonGetString(req.body, "ns");
    std::string name = JsonGetString(req.body, "name");
    std::string fieldName = JsonGetString(req.body, "field");
    std::string valueStr = JsonGetString(req.body, "value");
    if (name.empty() || fieldName.empty()) return JsonErr(400, "missing params");

    Il2CppClass* klass = Resolver::FindClass(ns.c_str(), name.c_str());
    if (!klass) return JsonErr(404, "class not found");

    FieldInfo* fi = il2cpp_class_get_field_from_name(klass, fieldName.c_str());
    if (!fi) return JsonErr(404, "field not found");
    if (!(il2cpp_field_get_flags(fi) & 0x10)) return JsonErr(400, "field is not static");

    const Il2CppType* ftype = il2cpp_field_get_type(fi);
    std::string typeName = GetTypeName(ftype);

    bool ok = false;
    if (typeName == "System.Int32" || typeName == "Int32" || typeName == "int") {
        int32_t val = std::stoi(valueStr);
        il2cpp_field_static_set_value(fi, &val); ok = true;
    } else if (typeName == "System.Int64" || typeName == "Int64" || typeName == "long") {
        int64_t val = std::stoll(valueStr);
        il2cpp_field_static_set_value(fi, &val); ok = true;
    } else if (typeName == "System.Single" || typeName == "Single" || typeName == "float") {
        float val = std::stof(valueStr);
        il2cpp_field_static_set_value(fi, &val); ok = true;
    } else if (typeName == "System.Double" || typeName == "Double" || typeName == "double") {
        double val = std::stod(valueStr);
        il2cpp_field_static_set_value(fi, &val); ok = true;
    } else if (typeName == "System.Boolean" || typeName == "Boolean" || typeName == "bool") {
        bool val = (valueStr == "true" || valueStr == "1");
        il2cpp_field_static_set_value(fi, &val); ok = true;
    } else {
        return JsonErr(400, "unsupported type for write: " + typeName);
    }

    return JsonOk("{\"ok\":" + std::string(ok ? "true" : "false") + "}");
}

// ─── Route: POST /api/invoke ───────────────────────────────────────────────
// Body: {"ns":"Game","class":"Foo","method":"Bar","args":["1","hello"],"instance":"0x1234"}

static WebServer::HttpResponse HandleInvokeImpl(const WebServer::HttpRequest& req) {
    std::string ns = JsonGetString(req.body, "ns");
    std::string className = JsonGetString(req.body, "class");
    std::string methodName = JsonGetString(req.body, "method");
    std::string instanceAddr = JsonGetString(req.body, "instance");
    auto args = JsonGetStringArray(req.body, "args");

    if (className.empty() || methodName.empty()) return JsonErr(400, "missing class/method");

    Il2CppClass* klass = Resolver::FindClass(ns.c_str(), className.c_str());
    if (!klass) return JsonErr(404, "class not found");

    const MethodInfo* method = il2cpp_class_get_method_from_name(klass, methodName.c_str(), (int)args.size());
    if (!method) {
        // Try with different arg counts
        method = il2cpp_class_get_method_from_name(klass, methodName.c_str(), 0);
        if (!method) return JsonErr(404, "method not found");
    }

    Il2CppObject* instance = nullptr;
    if (!instanceAddr.empty() && instanceAddr != "null" && instanceAddr != "0" && instanceAddr != "0x0") {
        uintptr_t iaddr = 0;
        try { iaddr = std::stoull(instanceAddr, nullptr, 16); } catch (...) {
            return JsonErr(400, "invalid instance address");
        }
        instance = reinterpret_cast<Il2CppObject*>(iaddr);
        if (!Resolver::Protection::IsValidIl2CppObject(instance))
            return JsonErr(400, "invalid instance object");
    }

    // Build params — convert string args to appropriate types
    std::vector<void*> params;
    std::vector<Il2CppObject*> boxedArgs; // prevent GC
    int paramCount = il2cpp_method_get_param_count(method);

    for (int i = 0; i < paramCount && i < (int)args.size(); i++) {
        const Il2CppType* ptype = il2cpp_method_get_param(method, i);
        std::string ptypeName = GetTypeName(ptype);
        const std::string& arg = args[i];

        if (ptypeName == "System.Int32" || ptypeName == "Int32" || ptypeName == "int") {
            int32_t val = std::stoi(arg);
            Il2CppClass* intClass = Resolver::FindClass("System", "Int32");
            Il2CppObject* boxed = il2cpp_value_box(intClass, &val);
            boxedArgs.push_back(boxed);
            params.push_back(il2cpp_object_unbox(boxed));
        } else if (ptypeName == "System.Int64" || ptypeName == "Int64" || ptypeName == "long") {
            int64_t val = std::stoll(arg);
            Il2CppClass* cls = Resolver::FindClass("System", "Int64");
            Il2CppObject* boxed = il2cpp_value_box(cls, &val);
            boxedArgs.push_back(boxed);
            params.push_back(il2cpp_object_unbox(boxed));
        } else if (ptypeName == "System.Single" || ptypeName == "Single" || ptypeName == "float") {
            float val = std::stof(arg);
            Il2CppClass* cls = Resolver::FindClass("System", "Single");
            Il2CppObject* boxed = il2cpp_value_box(cls, &val);
            boxedArgs.push_back(boxed);
            params.push_back(il2cpp_object_unbox(boxed));
        } else if (ptypeName == "System.Boolean" || ptypeName == "Boolean" || ptypeName == "bool") {
            bool val = (arg == "true" || arg == "1");
            Il2CppClass* cls = Resolver::FindClass("System", "Boolean");
            Il2CppObject* boxed = il2cpp_value_box(cls, &val);
            boxedArgs.push_back(boxed);
            params.push_back(il2cpp_object_unbox(boxed));
        } else if (ptypeName == "System.String" || ptypeName == "String") {
            Il2CppString* str = il2cpp_string_new(arg.c_str());
            params.push_back(str);
        } else {
            // Unsupported param type — try as pointer/null
            if (arg == "null" || arg.empty()) {
                params.push_back(nullptr);
            } else {
                return JsonErr(400, "unsupported param type: " + ptypeName + " at index " + std::to_string(i));
            }
        }
    }

    // Pad remaining params with nullptr
    while ((int)params.size() < paramCount) params.push_back(nullptr);

    Il2CppObject* result = Resolver::Protection::SafeRuntimeInvoke(method, instance, params.empty() ? nullptr : params.data());

    // Serialize result
    std::ostringstream js;
    js << "{\"ok\":true";
    if (result) {
        Il2CppClass* retClass = il2cpp_object_get_class(result);
        const char* retName = il2cpp_class_get_name(retClass);
        js << ",\"returnType\":" << JsonEsc(retName);

        // Try to stringify
        std::string tn = retName ? retName : "";
        if (tn == "Int32") {
            js << ",\"value\":" << *(int32_t*)il2cpp_object_unbox(result);
        } else if (tn == "Int64") {
            js << ",\"value\":" << *(int64_t*)il2cpp_object_unbox(result);
        } else if (tn == "Single") {
            js << ",\"value\":" << *(float*)il2cpp_object_unbox(result);
        } else if (tn == "Boolean") {
            js << ",\"value\":" << (*(bool*)il2cpp_object_unbox(result) ? "true" : "false");
        } else if (tn == "String") {
            Il2CppString* s = (Il2CppString*)result;
            auto* chars = il2cpp_string_chars(s);
            std::string utf8;
            for (int i = 0; i < s->length; i++) utf8 += (char)chars[i];
            js << ",\"value\":" << JsonStr(utf8);
        } else {
            char buf[32]; snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)result);
            js << ",\"value\":\"" << buf << "\"";
        }
    } else {
        js << ",\"value\":null";
    }
    js << "}";
    return JsonOk(js.str());
}

static WebServer::HttpResponse HandleInvoke(const WebServer::HttpRequest& req) {
    try {
        return HandleInvokeImpl(req);
    } catch (const std::exception& e) {
        return JsonErr(500, std::string("invoke error: ") + e.what());
    } catch (...) {
        return JsonErr(500, "unknown exception during invoke");
    }
}

// ─── Route: GET /api/status ────────────────────────────────────────────────

static WebServer::HttpResponse HandleStatus(const WebServer::HttpRequest& req) {
    std::ostringstream js;
    js << "{\"running\":true,\"clients\":" << WebServer::GetClientCount() << "}";
    return JsonOk(js.str());
}

// ─── Register all routes ────────────────────────────────────────────────────

void RegisterRoutes() {
    WebServer::Route("GET", "/api/assemblies", HandleAssemblies);
    WebServer::Route("GET", "/api/classes", HandleClasses);
    WebServer::Route("GET", "/api/class", HandleClass);
    WebServer::Route("GET", "/api/search", HandleSearch);
    WebServer::Route("GET", "/api/static", HandleStaticField);
    WebServer::Route("GET", "/api/msgdefs", HandleMsgDefs);
    WebServer::Route("GET", "/api/msgschema", HandleMsgSchema);
    WebServer::Route("GET", "/api/msgdump", HandleMsgDump);
    WebServer::Route("GET", "/api/prototags", HandleProtoTags);
    WebServer::Route("GET", "/api/memscan", HandleMemScan);
    WebServer::Route("GET", "/api/memdump", HandleMemDump);
    WebServer::Route("GET", "/api/methodinfo", HandleMethodInfo);
    WebServer::Route("GET", "/api/vtable", HandleVtable);
    WebServer::Route("GET", "/api/findpe", HandleFindPE);
    WebServer::Route("GET", "/api/dumppe", HandleDumpPE);
    WebServer::Route("GET", "/api/decode", HandleDecode);
    WebServer::Route("GET", "/api/protomap", HandleProtoMap);

    WebServer::Route("GET",  "/api/instances",   HandleInstances);
    WebServer::Route("GET",  "/api/readfield",   HandleReadField);
    WebServer::Route("POST", "/api/writefield",  HandleWriteField);
    WebServer::Route("POST", "/api/writestatic", HandleWriteStatic);
    WebServer::Route("POST", "/api/invoke",      HandleInvoke);
    WebServer::Route("GET",  "/api/status",      HandleStatus);

    std::cout << "[WebApi] Reflection API routes registered" << std::endl;
}

} // namespace ReflectionApi
