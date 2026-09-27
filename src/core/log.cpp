#include "core/log.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace spanly {
namespace {
std::mutex gLock;
FILE* gFile = nullptr;
} // namespace

void setLogFile(const std::string& path) {
    std::scoped_lock l(gLock);
    if (gFile) std::fclose(gFile);
    gFile = std::fopen(path.c_str(), "a");
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
    std::strftime(stamp, sizeof stamp, "%H:%M:%S", &t);
    std::scoped_lock l(gLock);
    std::fprintf(stderr, "[%s] %s\n", stamp, message.c_str());
    if (gFile) {
        std::fprintf(gFile, "[%s] %s\n", stamp, message.c_str());
        std::fflush(gFile);
    }
}

} // namespace spanly
