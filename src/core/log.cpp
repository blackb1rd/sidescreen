#include "core/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace spanly {
namespace {
std::mutex gLock;
FILE* gFile = nullptr;
bool gStderr = true;
} // namespace

void setLogFile(const std::string& path) {
    std::scoped_lock l(gLock);
    if (gFile) (void)std::fclose(gFile);
#ifdef _WIN32
    if (fopen_s(&gFile, path.c_str(), "a") != 0) gFile = nullptr;
#else
    gFile = std::fopen(path.c_str(), "a");
#endif
}

void setLogToStderr(bool on) {
    std::scoped_lock l(gLock);
    gStderr = on;
}

void logLine(const std::string& message) {
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm t{};
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char stamp[16];
    (void)std::strftime(stamp, sizeof stamp, "%H:%M:%S", &t);
    std::scoped_lock l(gLock);
    if (gStderr) (void)std::fprintf(stderr, "[%s] %s\n", stamp, message.c_str());
    if (gFile) {
        (void)std::fprintf(gFile, "[%s] %s\n", stamp, message.c_str());
        (void)std::fflush(gFile);
    }
}

} // namespace spanly
