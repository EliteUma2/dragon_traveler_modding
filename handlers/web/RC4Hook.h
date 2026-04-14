#pragma once
#include <string>

namespace RC4Hook {
    // Install Detours hooks on RC4.Encrypt and TcpClient.ReceivePayload
    void Install();
    void Uninstall();

    // Register /api/rc4debug route
    void RegisterRoutes();

    // Get JSON log of last N RC4 operations
    std::string GetLog();
}
