// Running helper programs (adb) on macOS and Linux.
#include "core/env.hpp"
#include "platform/platform.hpp"

#include <array>
#include <cstdlib>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ; // NOLINT(readability-redundant-declaration): not declared by every libc

namespace spanly::platform {

std::string runProcess(const std::string& path, const std::vector<std::string>& args) {
    std::array<int, 2> out{};
    if (pipe(out.data()) != 0) return {};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    posix_spawn_file_actions_adddup2(&actions, out[1], 2);
    posix_spawn_file_actions_addclose(&actions, out[0]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(path.c_str()));
    for (const auto& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    int err = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(out[1]);
    std::string output;
    if (err == 0) {
        std::array<char, 4096> buf{};
        ssize_t n = 0;
        while ((n = read(out[0], buf.data(), buf.size())) > 0)
            output.append(buf.data(), size_t(n));
        int status = 0;
        waitpid(pid, &status, 0);
    }
    close(out[0]);
    return output;
}

bool isExecutable(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), X_OK) == 0;
}

std::string homeDirectory() {
    return env("HOME").value_or("/");
}

} // namespace spanly::platform
