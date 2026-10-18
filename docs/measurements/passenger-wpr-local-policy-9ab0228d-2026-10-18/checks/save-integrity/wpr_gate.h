#pragma once
#include <chrono>
#include <fstream>
#include <thread>
#include <windows.h>
namespace PassengerWpr {
inline bool Gate(const char *ready, const char *release)
{
    if (getenv("OPENTTD_WPR_PROFILE") == nullptr) return true;
    { std::ofstream out(ready); out << GetCurrentProcessId() << ' ' << GetCurrentThreadId() << std::endl; }
    const auto until = std::chrono::steady_clock::now() + std::chrono::minutes(15);
    while (std::chrono::steady_clock::now() < until) {
        if (std::ifstream(release).good()) return true;
        if (std::ifstream("wpr-error.txt").good()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
}
