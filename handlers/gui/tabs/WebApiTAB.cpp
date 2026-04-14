#include "pch-il2cpp.h"
#include "WebApiTAB.h"
#include <imgui/imgui.h>
#include "web/WebServer.h"
#include "web/PacketCapture.h"
#include "web/ReflectionApi.h"
#include "web/ReverseApi.h"
#include "web/RC4Hook.h"
#include "web/PriceHook.h"
#include <string>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <deque>
#include <Windows.h>
#include <shellapi.h>

namespace WebApiTAB {

static int  s_port = 6969;

// Broadcast thread: drains captured packets and sends them to WS clients
static std::atomic<bool> s_broadcastRunning{false};
static std::thread s_broadcastThread;

// ─── Packet ring buffer for REST polling ────────────────────────────────────
static const size_t RING_MAX = 1000;

struct RingEntry {
    uint32_t seq;
    std::string dir;
    int32_t msgId;
    int32_t len;
    int32_t dataLen;
    uint64_t timestamp;
    std::string hexData;
};

static std::mutex s_ringMutex;
static std::deque<RingEntry> s_ring;

// Simple JSON escape
static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
        }
    }
    return out;
}

static std::string BytesToHex(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        out += hex[data[i] >> 4];
        out += hex[data[i] & 0xF];
    }
    return out;
}

static void BroadcastThreadFunc() {
    // Attach to IL2CPP domain so we can safely call managed code (SendPacket queue)
    auto* thread = il2cpp_thread_attach(il2cpp_domain_get());

    while (s_broadcastRunning.load()) {
        // Process any queued send/recv requests (must be on IL2CPP-attached thread)
        PacketCapture::ProcessSendQueue();
        PacketCapture::ProcessRecvQueue();

        auto packets = PacketCapture::Drain();
        for (auto& pkt : packets) {
            std::string hex = BytesToHex(pkt.data.data(), pkt.data.size());
            std::string dirStr = (pkt.dir == PacketCapture::CapturedPacket::SEND) ? "send" : "recv";

            // Store in ring buffer for REST polling
            {
                std::lock_guard<std::mutex> lk(s_ringMutex);
                s_ring.push_back({pkt.seq, dirStr, pkt.msgId, pkt.len, pkt.dataLen, pkt.timestamp, hex});
                while (s_ring.size() > RING_MAX) s_ring.pop_front();
            }

            // Broadcast to WS clients
            if (WebServer::IsRunning() && WebServer::GetClientCount() > 0) {
                std::string json = "{\"type\":\"packet\""
                    ",\"dir\":\"" + dirStr + "\""
                    ",\"msgId\":" + std::to_string(pkt.msgId) +
                    ",\"len\":" + std::to_string(pkt.len) +
                    ",\"dataLen\":" + std::to_string(pkt.dataLen) +
                    ",\"seq\":" + std::to_string(pkt.seq) +
                    ",\"ts\":" + std::to_string(pkt.timestamp) +
                    ",\"data\":\"" + hex + "\"}";
                WebServer::BroadcastWS(json);
            }
        }
        Sleep(50); // ~20Hz
    }

    // Detach from IL2CPP so the runtime can shut down cleanly
    if (thread) il2cpp_thread_detach(thread);
}

// WS message handler (from browser)
static void OnWsMessage(uint32_t clientId, const std::string& msg) {
    // Simple JSON parsing for {"type":"send","msgId":N,"data":"hex"}
    // Find type
    auto typePos = msg.find("\"type\"");
    if (typePos == std::string::npos) return;

    // Helper: parse msgId and hex data from JSON message
    auto parseMsgIdAndData = [&](int32_t& msgId, std::vector<uint8_t>& data) -> bool {
        auto midPos = msg.find("\"msgId\"");
        if (midPos == std::string::npos) return false;
        auto colon = msg.find(':', midPos + 7);
        if (colon == std::string::npos) return false;
        msgId = std::atoi(msg.c_str() + colon + 1);

        auto dataPos = msg.find("\"data\"");
        if (dataPos == std::string::npos) return false;
        auto q1 = msg.find('"', dataPos + 6);
        if (q1 == std::string::npos) return false;
        q1++;
        auto q2 = msg.find('"', q1);
        if (q2 == std::string::npos) return false;
        std::string hexStr = msg.substr(q1, q2 - q1);

        data.reserve(hexStr.size() / 2);
        for (size_t i = 0; i + 1 < hexStr.size(); i += 2) {
            uint8_t b = (uint8_t)strtol(hexStr.substr(i, 2).c_str(), nullptr, 16);
            data.push_back(b);
        }
        return !data.empty();
    };

    if (msg.find("\"send\"") != std::string::npos) {
        // Send: data is full transport frame (20-byte header + proto)
        int32_t msgId;
        std::vector<uint8_t> data;
        if (parseMsgIdAndData(msgId, data)) {
            PacketCapture::QueueSend(msgId, data.data(), (int32_t)data.size());
        }
    }
    else if (msg.find("\"recv\"") != std::string::npos) {
        // Recv simulation: data is raw proto bytes (no transport frame)
        int32_t msgId;
        std::vector<uint8_t> data;
        if (parseMsgIdAndData(msgId, data)) {
            PacketCapture::QueueRecv(msgId, data.data(), (int32_t)data.size());
        }
    }
}

// ─── REST endpoint: GET /api/packets?since=N&limit=M ────────────────────────
static WebServer::HttpResponse HandlePackets(const WebServer::HttpRequest& req) {
    uint32_t since = 0;
    size_t limit = 200;

    auto sinceIt = req.params.find("since");
    if (sinceIt != req.params.end()) since = (uint32_t)std::stoul(sinceIt->second);
    auto limitIt = req.params.find("limit");
    if (limitIt != req.params.end()) {
        limit = (size_t)std::stoul(limitIt->second);
        if (limit > 1000) limit = 1000;
    }

    std::string json = "[";
    bool first = true;
    {
        std::lock_guard<std::mutex> lk(s_ringMutex);
        size_t count = 0;
        for (auto& e : s_ring) {
            if (e.seq <= since) continue;
            if (count >= limit) break;
            if (!first) json += ",";
            first = false;
            json += "{\"seq\":" + std::to_string(e.seq) +
                    ",\"dir\":\"" + e.dir + "\""
                    ",\"msgId\":" + std::to_string(e.msgId) +
                    ",\"len\":" + std::to_string(e.len) +
                    ",\"dataLen\":" + std::to_string(e.dataLen) +
                    ",\"ts\":" + std::to_string(e.timestamp) +
                    ",\"data\":\"" + e.hexData + "\"}";
            count++;
        }
    }
    json += "]";
    return {200, "application/json", json};
}

// Build webRoot path relative to DLL location
static std::string GetWebRoot() {
    // Try DLL directory first
    char dllPath[MAX_PATH] = {};
    HMODULE hMod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&GetWebRoot, &hMod);
    GetModuleFileNameA(hMod, dllPath, MAX_PATH);
    std::string path(dllPath);
    auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(0, pos);

    // Check if web/index.html exists next to DLL
    std::string candidate = path + "\\web";
    DWORD attr = GetFileAttributesA((candidate + "\\index.html").c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return candidate;

    // Fallback: project source directory
    attr = GetFileAttributesA("d:\\dp\\web\\index.html");
    if (attr != INVALID_FILE_ATTRIBUTES) return "d:\\dp\\web";

    return candidate; // original default even if not found
}

void Render() {
    ImGui::Text("WEB Api");
    ImGui::Separator();
    ImGui::Spacing();

    // Enable / Disable toggle
    bool running = WebServer::IsRunning();
    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
        if (ImGui::Button("Disable", ImVec2(120, 28))) {
            // Stop broadcast thread
            s_broadcastRunning.store(false);
            if (s_broadcastThread.joinable()) s_broadcastThread.join();

            PacketCapture::Disable();
            WebServer::Stop();
            { std::lock_guard<std::mutex> lk(s_ringMutex); s_ring.clear(); }
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.7f, 0.3f, 1.0f));
        if (ImGui::Button("Enable", ImVec2(120, 28))) {
            // Register reflection API routes + start web server
            ReflectionApi::RegisterRoutes();
            ReverseApi::RegisterRoutes();
            RC4Hook::RegisterRoutes();
            PriceHook::RegisterRoutes();
            WebServer::Route("GET", "/api/packets", HandlePackets);
            std::string webRoot = GetWebRoot();
            std::cout << "[WebApi] Web root: " << webRoot << std::endl;
            WebServer::SetWsHandler(OnWsMessage);
            WebServer::Start((uint16_t)s_port, webRoot);

            // Enable packet capture hooks
            PacketCapture::Enable();

            // Start broadcast thread
            s_broadcastRunning.store(true);
            s_broadcastThread = std::thread(BroadcastThreadFunc);
        }
        ImGui::PopStyleColor();
    }

    // Status
    ImGui::SameLine();
    std::string status = WebServer::GetStatus();
    if (running) {
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "Status: %s", status.c_str());
    } else {
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Status: %s", status.c_str());
    }

    ImGui::Spacing();

    // Port input (only when stopped)
    if (!running) {
        ImGui::SetNextItemWidth(80);
        ImGui::InputInt("Port", &s_port, 0, 0);
        if (s_port < 1) s_port = 1;
        if (s_port > 65535) s_port = 65535;
    }

    if (running) {
        ImGui::Text("Clients: %u", WebServer::GetClientCount());
        ImGui::Text("Packets: S:%u  R:%u",
                     PacketCapture::GetSendCount(), PacketCapture::GetRecvCount());

        ImGui::Spacing();
        if (ImGui::Button("Open in Browser", ImVec2(150, 28))) {
            std::string url = "http://localhost:" + std::to_string(s_port);
            ShellExecuteA(NULL, "open", url.c_str(), NULL, NULL, SW_SHOWNORMAL);
        }
    }
}

void Shutdown() {
    if (s_broadcastRunning.load()) {
        s_broadcastRunning.store(false);
        if (s_broadcastThread.joinable()) s_broadcastThread.join();
    }
    PacketCapture::Disable();
    WebServer::Stop();
}

} // namespace WebApiTAB
