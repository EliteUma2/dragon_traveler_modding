#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "WebServer.h"

#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <sstream>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <unordered_map>

// ─── WinCrypt for SHA-1 (WebSocket handshake) ───
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")

namespace WebServer {

// ─── Internal state ─────────────────────────────────────────────────────────

static std::atomic<bool>    s_running{false};
static std::atomic<bool>    s_stopping{false};
static SOCKET               s_listenSock = INVALID_SOCKET;
static std::thread          s_acceptThread;
static uint16_t             s_port = 6969;
static std::string          s_webRoot;
static std::string          s_status = "Stopped";
static std::mutex           s_statusMutex;

// Routes
struct RouteEntry {
    std::string method; // "GET", "POST", "*"
    std::string path;   // prefix match
    HttpHandler handler;
};
static std::mutex               s_routesMutex;
static std::vector<RouteEntry>  s_routes;

// WebSocket clients
struct WsClient {
    uint32_t    id;
    SOCKET      sock;
    std::thread thread;
    std::atomic<bool> alive{true};
};
static std::mutex                                s_wsMutex;
static std::vector<std::shared_ptr<WsClient>>    s_wsClients;
static std::atomic<uint32_t>                     s_nextClientId{1};
static WsHandler                                 s_wsHandler;

// ─── Helpers ────────────────────────────────────────────────────────────────

static void SetStatus(const std::string& s) {
    std::lock_guard<std::mutex> lk(s_statusMutex);
    s_status = s;
}

static std::string GetMimeType(const std::string& path) {
    auto ext = path.substr(path.find_last_of('.') + 1);
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "js")   return "application/javascript; charset=utf-8";
    if (ext == "css")  return "text/css; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "png")  return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "svg")  return "image/svg+xml";
    if (ext == "ico")  return "image/x-icon";
    return "application/octet-stream";
}

// Parse "GET /path?a=1&b=2 HTTP/1.1\r\n..." into HttpRequest
static bool ParseHttpRequest(const std::string& raw, HttpRequest& req) {
    // Request line
    auto lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) return false;
    std::string requestLine = raw.substr(0, lineEnd);

    // Method
    auto sp1 = requestLine.find(' ');
    if (sp1 == std::string::npos) return false;
    req.method = requestLine.substr(0, sp1);

    // URI
    auto sp2 = requestLine.find(' ', sp1 + 1);
    if (sp2 == std::string::npos) return false;
    std::string uri = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

    // Path and query string
    auto qmark = uri.find('?');
    if (qmark != std::string::npos) {
        req.path = uri.substr(0, qmark);
        std::string qs = uri.substr(qmark + 1);
        // Parse query params
        std::istringstream ss(qs);
        std::string pair;
        while (std::getline(ss, pair, '&')) {
            auto eq = pair.find('=');
            if (eq != std::string::npos)
                req.params[pair.substr(0, eq)] = pair.substr(eq + 1);
            else
                req.params[pair] = "";
        }
    } else {
        req.path = uri;
    }

    // Headers
    size_t pos = lineEnd + 2;
    while (pos < raw.size()) {
        auto nextLine = raw.find("\r\n", pos);
        if (nextLine == std::string::npos || nextLine == pos) break; // empty line = end of headers
        std::string header = raw.substr(pos, nextLine - pos);
        auto colon = header.find(':');
        if (colon != std::string::npos) {
            std::string key = header.substr(0, colon);
            std::string val = header.substr(colon + 1);
            // Trim leading spaces
            while (!val.empty() && val[0] == ' ') val.erase(0, 1);
            // Lowercase key for easy comparison
            std::string keyLower = key;
            std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);
            req.headers[keyLower] = val;
        }
        pos = nextLine + 2;
    }

    // Body (after \r\n\r\n)
    auto bodyStart = raw.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        req.body = raw.substr(bodyStart + 4);
    }

    return true;
}

static std::string BuildHttpResponse(int status, const std::string& contentType,
                                     const std::string& body,
                                     const std::map<std::string, std::string>& extraHeaders = {}) {
    std::string statusText;
    switch (status) {
        case 200: statusText = "OK"; break;
        case 101: statusText = "Switching Protocols"; break;
        case 400: statusText = "Bad Request"; break;
        case 404: statusText = "Not Found"; break;
        case 405: statusText = "Method Not Allowed"; break;
        case 500: statusText = "Internal Server Error"; break;
        default:  statusText = "Unknown"; break;
    }
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " " << statusText << "\r\n";
    oss << "Content-Type: " << contentType << "\r\n";
    oss << "Content-Length: " << body.size() << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    oss << "Access-Control-Allow-Headers: Content-Type\r\n";
    oss << "Connection: close\r\n";
    for (auto& [k, v] : extraHeaders) {
        oss << k << ": " << v << "\r\n";
    }
    oss << "\r\n";
    oss << body;
    return oss.str();
}

// Read full HTTP request from socket (up to 64KB)
static std::string RecvHttpRequest(SOCKET sock) {
    std::string buf;
    buf.resize(65536);
    int total = 0;
    while (total < (int)buf.size() - 1) {
        int n = recv(sock, &buf[total], (int)buf.size() - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        // Check if we have the full headers
        if (buf.find("\r\n\r\n") != std::string::npos) {
            // Check Content-Length for body
            std::string lower(buf.begin(), buf.begin() + total);
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            auto clPos = lower.find("content-length:");
            if (clPos != std::string::npos) {
                int cl = std::atoi(lower.c_str() + clPos + 15);
                auto bodyStart = buf.find("\r\n\r\n");
                int bodyReceived = total - (int)(bodyStart + 4);
                if (bodyReceived >= cl) break;
                // Need more body data, continue reading
            } else {
                break; // No body expected
            }
        }
    }
    buf.resize(total);
    return buf;
}

// ─── Static file serving ────────────────────────────────────────────────────

static bool ServeStaticFile(SOCKET sock, const std::string& urlPath) {
    if (s_webRoot.empty()) return false;

    std::string filePath = urlPath;
    if (filePath == "/") filePath = "/index.html";

    // Security: prevent path traversal
    if (filePath.find("..") != std::string::npos) return false;

    std::string fullPath = s_webRoot + filePath;
    // Normalize separators
    std::replace(fullPath.begin(), fullPath.end(), '/', '\\');

    std::ifstream file(fullPath, std::ios::binary);
    if (!file.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());
    file.close();

    std::string mime = GetMimeType(fullPath);
    std::string resp = BuildHttpResponse(200, mime, content);
    send(sock, resp.c_str(), (int)resp.size(), 0);
    return true;
}

// ─── WebSocket ──────────────────────────────────────────────────────────────

static std::string SHA1(const std::string& input) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    std::string result;

    if (!CryptAcquireContextA(&hProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return result;

    if (!CryptCreateHash(hProv, CALG_SHA1, 0, 0, &hHash)) {
        CryptReleaseContext(hProv, 0);
        return result;
    }

    CryptHashData(hHash, (const BYTE*)input.data(), (DWORD)input.size(), 0);

    DWORD hashLen = 20;
    result.resize(hashLen);
    CryptGetHashParam(hHash, HP_HASHVAL, (BYTE*)result.data(), &hashLen, 0);

    CryptDestroyHash(hHash);
    CryptReleaseContext(hProv, 0);
    return result;
}

static std::string Base64Encode(const std::string& input) {
    DWORD outLen = 0;
    CryptBinaryToStringA((const BYTE*)input.data(), (DWORD)input.size(),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &outLen);
    std::string out(outLen, '\0');
    CryptBinaryToStringA((const BYTE*)input.data(), (DWORD)input.size(),
                         CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, &out[0], &outLen);
    // Trim trailing nulls/whitespace
    while (!out.empty() && (out.back() == '\0' || out.back() == '\r' || out.back() == '\n'))
        out.pop_back();
    return out;
}

static bool DoWsHandshake(SOCKET sock, const HttpRequest& req) {
    auto it = req.headers.find("sec-websocket-key");
    if (it == req.headers.end()) return false;

    std::string acceptKey = it->second + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string sha = SHA1(acceptKey);
    std::string b64 = Base64Encode(sha);

    std::ostringstream oss;
    oss << "HTTP/1.1 101 Switching Protocols\r\n";
    oss << "Upgrade: websocket\r\n";
    oss << "Connection: Upgrade\r\n";
    oss << "Sec-WebSocket-Accept: " << b64 << "\r\n";
    oss << "\r\n";
    std::string resp = oss.str();
    return send(sock, resp.c_str(), (int)resp.size(), 0) > 0;
}

// Send a WS text frame
static bool WsSendFrame(SOCKET sock, const std::string& payload) {
    std::vector<uint8_t> frame;
    frame.push_back(0x81); // FIN + text opcode

    if (payload.size() < 126) {
        frame.push_back((uint8_t)payload.size());
    } else if (payload.size() <= 0xFFFF) {
        frame.push_back(126);
        frame.push_back((uint8_t)(payload.size() >> 8));
        frame.push_back((uint8_t)(payload.size() & 0xFF));
    } else {
        frame.push_back(127);
        uint64_t len = payload.size();
        for (int i = 7; i >= 0; i--)
            frame.push_back((uint8_t)(len >> (i * 8)));
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return send(sock, (const char*)frame.data(), (int)frame.size(), 0) > 0;
}

// Read a WS frame, returns payload (text). Returns empty on close/error.
static std::string WsRecvFrame(SOCKET sock) {
    uint8_t header[2];
    if (recv(sock, (char*)header, 2, MSG_WAITALL) != 2) return "";

    uint8_t opcode = header[0] & 0x0F;
    bool masked = (header[1] & 0x80) != 0;
    uint64_t payloadLen = header[1] & 0x7F;

    if (payloadLen == 126) {
        uint8_t ext[2];
        if (recv(sock, (char*)ext, 2, MSG_WAITALL) != 2) return "";
        payloadLen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payloadLen == 127) {
        uint8_t ext[8];
        if (recv(sock, (char*)ext, 8, MSG_WAITALL) != 8) return "";
        payloadLen = 0;
        for (int i = 0; i < 8; i++)
            payloadLen = (payloadLen << 8) | ext[i];
    }

    uint8_t mask[4] = {};
    if (masked) {
        if (recv(sock, (char*)mask, 4, MSG_WAITALL) != 4) return "";
    }

    if (payloadLen > 16 * 1024 * 1024) return ""; // 16MB limit

    std::string payload(payloadLen, '\0');
    uint64_t received = 0;
    while (received < payloadLen) {
        int n = recv(sock, &payload[(size_t)received], (int)(payloadLen - received), 0);
        if (n <= 0) return "";
        received += n;
    }

    if (masked) {
        for (uint64_t i = 0; i < payloadLen; i++)
            payload[i] ^= mask[i % 4];
    }

    // Handle opcodes
    if (opcode == 0x8) return ""; // close
    if (opcode == 0x9) {
        // Ping → send pong
        std::vector<uint8_t> pong;
        pong.push_back(0x8A); // FIN + pong
        pong.push_back((uint8_t)payloadLen);
        pong.insert(pong.end(), payload.begin(), payload.end());
        send(sock, (const char*)pong.data(), (int)pong.size(), 0);
        return WsRecvFrame(sock); // read next frame
    }

    return payload;
}

static void WsClientThread(std::shared_ptr<WsClient> client) {
    while (client->alive.load() && s_running.load()) {
        std::string msg = WsRecvFrame(client->sock);
        if (msg.empty()) break;
        if (s_wsHandler) {
            try { s_wsHandler(client->id, msg); } catch (...) {}
        }
    }
    client->alive.store(false);
    closesocket(client->sock);
}

// ─── Client connection handler ──────────────────────────────────────────────

static void HandleClient(SOCKET sock) {
    std::string rawReq = RecvHttpRequest(sock);
    if (rawReq.empty()) { closesocket(sock); return; }

    HttpRequest req;
    if (!ParseHttpRequest(rawReq, req)) {
        closesocket(sock);
        return;
    }

    // OPTIONS preflight
    if (req.method == "OPTIONS") {
        std::string resp = BuildHttpResponse(200, "text/plain", "");
        send(sock, resp.c_str(), (int)resp.size(), 0);
        closesocket(sock);
        return;
    }

    // WebSocket upgrade?
    auto upgradeIt = req.headers.find("upgrade");
    if (upgradeIt != req.headers.end()) {
        std::string val = upgradeIt->second;
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        if (val == "websocket") {
            if (DoWsHandshake(sock, req)) {
                auto client = std::make_shared<WsClient>();
                client->id = s_nextClientId.fetch_add(1);
                client->sock = sock;
                client->alive.store(true);
                client->thread = std::thread(WsClientThread, client);
                client->thread.detach();
                {
                    std::lock_guard<std::mutex> lk(s_wsMutex);
                    s_wsClients.push_back(client);
                }
                return; // Don't close socket — WS thread owns it
            }
            closesocket(sock);
            return;
        }
    }

    // Try registered routes
    {
        std::lock_guard<std::mutex> lk(s_routesMutex);
        for (auto& route : s_routes) {
            if ((route.method == "*" || route.method == req.method) &&
                (req.path == route.path || req.path.find(route.path) == 0)) {
                try {
                    HttpResponse httpResp = route.handler(req);
                    std::string resp = BuildHttpResponse(httpResp.status, httpResp.contentType,
                                                         httpResp.body, httpResp.headers);
                    send(sock, resp.c_str(), (int)resp.size(), 0);
                } catch (const std::exception& e) {
                    std::string resp = BuildHttpResponse(500, "text/plain",
                                                         std::string("Error: ") + e.what());
                    send(sock, resp.c_str(), (int)resp.size(), 0);
                }
                closesocket(sock);
                return;
            }
        }
    }

    // Try static file
    if (req.method == "GET" && ServeStaticFile(sock, req.path)) {
        closesocket(sock);
        return;
    }

    // 404
    std::string resp = BuildHttpResponse(404, "text/plain", "Not Found");
    send(sock, resp.c_str(), (int)resp.size(), 0);
    closesocket(sock);
}

// ─── Accept loop ────────────────────────────────────────────────────────────

static void AcceptLoop() {
    SetStatus("Running on :" + std::to_string(s_port));
    while (s_running.load()) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(s_listenSock, &readSet);
        timeval tv = {0, 200000}; // 200ms timeout for checking s_running
        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        SOCKET clientSock = accept(s_listenSock, nullptr, nullptr);
        if (clientSock == INVALID_SOCKET) continue;

        // Handle in a detached thread (short-lived for HTTP, long-lived for WS)
        std::thread(HandleClient, clientSock).detach();
    }
    SetStatus("Stopped");
}

// ─── Public API ─────────────────────────────────────────────────────────────

void Start(uint16_t port, const std::string& webRoot) {
    if (s_running.load()) return;

    s_port = port;
    s_webRoot = webRoot;
    s_stopping.store(false);

    // Init WinSock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        SetStatus("WSAStartup failed");
        return;
    }

    s_listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listenSock == INVALID_SOCKET) {
        SetStatus("socket() failed");
        WSACleanup();
        return;
    }

    // Allow reuse
    int opt = 1;
    setsockopt(s_listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(s_listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        SetStatus("bind() failed — port " + std::to_string(port) + " in use?");
        closesocket(s_listenSock);
        s_listenSock = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    if (listen(s_listenSock, SOMAXCONN) == SOCKET_ERROR) {
        SetStatus("listen() failed");
        closesocket(s_listenSock);
        s_listenSock = INVALID_SOCKET;
        WSACleanup();
        return;
    }

    s_running.store(true);
    s_acceptThread = std::thread(AcceptLoop);
    s_acceptThread.detach();
}

void Stop() {
    if (!s_running.load()) return;
    s_stopping.store(true);
    s_running.store(false);

    // Close listen socket to unblock accept
    if (s_listenSock != INVALID_SOCKET) {
        closesocket(s_listenSock);
        s_listenSock = INVALID_SOCKET;
    }

    // Close all WS clients
    {
        std::lock_guard<std::mutex> lk(s_wsMutex);
        for (auto& c : s_wsClients) {
            c->alive.store(false);
            closesocket(c->sock);
        }
        s_wsClients.clear();
    }

    WSACleanup();
    SetStatus("Stopped");
}

bool IsRunning() {
    return s_running.load();
}

std::string GetStatus() {
    std::lock_guard<std::mutex> lk(s_statusMutex);
    return s_status;
}

uint32_t GetClientCount() {
    std::lock_guard<std::mutex> lk(s_wsMutex);
    // Clean up dead clients while we're here
    s_wsClients.erase(
        std::remove_if(s_wsClients.begin(), s_wsClients.end(),
                       [](const std::shared_ptr<WsClient>& c) { return !c->alive.load(); }),
        s_wsClients.end());
    return (uint32_t)s_wsClients.size();
}

void Route(const std::string& method, const std::string& path, HttpHandler handler) {
    std::lock_guard<std::mutex> lk(s_routesMutex);
    s_routes.push_back({method, path, handler});
}

void BroadcastWS(const std::string& json) {
    std::lock_guard<std::mutex> lk(s_wsMutex);
    for (auto it = s_wsClients.begin(); it != s_wsClients.end(); ) {
        if (!(*it)->alive.load()) {
            it = s_wsClients.erase(it);
            continue;
        }
        if (!WsSendFrame((*it)->sock, json)) {
            (*it)->alive.store(false);
            it = s_wsClients.erase(it);
            continue;
        }
        ++it;
    }
}

void SetWsHandler(WsHandler handler) {
    s_wsHandler = handler;
}

void SendWS(uint32_t clientId, const std::string& json) {
    std::lock_guard<std::mutex> lk(s_wsMutex);
    for (auto& c : s_wsClients) {
        if (c->id == clientId && c->alive.load()) {
            WsSendFrame(c->sock, json);
            return;
        }
    }
}

} // namespace WebServer
