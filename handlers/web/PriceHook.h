#pragma once
#include <string>

namespace PriceHook {
    void Install();
    void Uninstall();
    void RegisterRoutes();
    std::string GetLog();
}
