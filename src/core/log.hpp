#pragma once
#include <format>
#include <string>

namespace spanly {

/// Write "[HH:MM:SS] message" to stderr and the log file (see setLogFile).
void logLine(const std::string& message);

template <class... Args>
void log(std::format_string<Args...> fmt, Args&&... args) {
    logLine(std::format(fmt, std::forward<Args>(args)...));
}

/// Also append log lines to this file (e.g. ~/Library/Logs/Spanly.log).
void setLogFile(const std::string& path);

/// Off: nothing goes to stderr (the fuzzers, which log millions of lines otherwise).
void setLogToStderr(bool on);

} // namespace spanly
