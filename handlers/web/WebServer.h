#pragma once
#include <string>
#include <functional>
#include <map>
#include <cstdint>

namespace WebServer {

    struct HttpRequest {
        std::string method;
        std::string path;
        std::string body;
        std::map<std::string, std::string> params;   // query params
        std::map<std::string, std::string> headers;
    };

    struct HttpResponse {
        int status = 200;
        std::string contentType = "application/json";
        std::string body;
        std::map<std::string, std::string> headers;
    };

    using HttpHandler = std::function<HttpResponse(const HttpRequest&)>;

    // Lifecycle
    void Start(uint16_t port = 6969, const std::string& webRoot = "");
    void Stop();
    bool IsRunning();
    std::string GetStatus();
    uint32_t GetClientCount();

    // Route registration (call before or after Start)
    void Route(const std::string& method, const std::string& path, HttpHandler handler);

    // WebSocket broadcast to all connected WS clients
    void BroadcastWS(const std::string& json);

    // WebSocket message handler (set before Start)
    using WsHandler = std::function<void(uint32_t clientId, const std::string& msg)>;
    void SetWsHandler(WsHandler handler);

    // Send to specific WS client
    void SendWS(uint32_t clientId, const std::string& json);
}
