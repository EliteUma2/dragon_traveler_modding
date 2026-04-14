#pragma once

namespace ReflectionApi {
    // Register all IL2CPP reflection routes on WebServer
    // Call once before WebServer::Start()
    void RegisterRoutes();
}
