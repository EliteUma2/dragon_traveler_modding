#pragma once

namespace ReverseApi {
    // Register RE/system introspection routes on WebServer
    // Call once before WebServer::Start()
    void RegisterRoutes();
}
