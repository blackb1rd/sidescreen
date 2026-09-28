// Windows: the message loop (main thread), paths, power and display events, the login item and
// running helper programs.
#include "core/env.hpp"
#include "core/log.hpp"
#include "platform/platform.hpp"
#include "platform/windows/win.hpp"

#include <shellapi.h>

#include <cstdlib>
#include <filesystem>
#include <map>
#include <vector>

namespace spanly::platform {

namespace fs = std::filesystem;

bool supports(Feature f) {
    return f == Feature::Tray;
}

namespace win {

namespace {
HWND gWindow = nullptr;
std::map<UINT, std::vector<std::function<void(WPARAM, LPARAM)>>> gHandlers;
std::map<UINT_PTR, std::function<void()>> gTimers;
UINT_PTR gNextTimer = 1;

LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    if (msg == kRunMessage) {
        std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(l));
        (*fn)();
        return 0;
    }
    if (msg == WM_TIMER) {
        KillTimer(hwnd, w);
        auto it = gTimers.find(w);
        if (it != gTimers.end()) {
            auto fn = std::move(it->second);
            gTimers.erase(it);
            fn();
        }
        return 0;
    }
    if (auto it = gHandlers.find(msg); it != gHandlers.end()) {
        for (auto& h : it->second)
            h(w, l);
        if (msg == WM_POWERBROADCAST) return TRUE;
    }
    return DefWindowProcW(hwnd, msg, w, l);
}
} // namespace

HWND window() {
    if (!gWindow) {
        WNDCLASSW c{};
        c.lpfnWndProc = proc;
        c.hInstance = GetModuleHandleW(nullptr);
        c.lpszClassName = L"SpanlyMain";
        RegisterClassW(&c);
        gWindow = CreateWindowExW(0, c.lpszClassName, L"Spanly", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                  c.hInstance, nullptr); // never shown
    }
    return gWindow;
}

void onMessage(UINT message, std::function<void(WPARAM, LPARAM)> handler) {
    gHandlers[message].push_back(std::move(handler));
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace win

// MARK: Main thread

namespace {
SystemEvents gEvents;
}

int runApp(const std::function<void()>& start) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // pixels, not scaled points
    win::window();
    runOnMain(start);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (gEvents.willQuit) gEvents.willQuit();
    return 0;
}

void quit() {
    runOnMain([] { PostQuitMessage(0); });
}

void runOnMain(std::function<void()> fn) {
    PostMessageW(win::window(), win::kRunMessage, 0,
                 reinterpret_cast<LPARAM>(new std::function<void()>(std::move(fn))));
}

void runAfter(double seconds, std::function<void()> fn) {
    runOnMain([seconds, fn = std::move(fn)]() mutable {
        UINT_PTR id = win::gNextTimer++;
        win::gTimers[id] = std::move(fn);
        SetTimer(win::window(), id, UINT(seconds * 1000), nullptr);
    });
}

// MARK: Permissions (none needed on Windows)

bool screenRecordingAllowed() {
    return true;
}
void requestScreenRecording() {}
bool accessibilityAllowed() {
    return true;
}
void requestAccessibility() {}

// MARK: Names and paths

std::string computerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    GetComputerNameW(name, &size);
    return win::utf8(name);
}

namespace {
std::string knownDir(const char* var) {
    std::string dir = env(var).value_or(".") + "\\Spanly";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}
} // namespace

std::string configDirectory() {
    return knownDir("APPDATA");
}

std::string logFilePath() {
    return knownDir("LOCALAPPDATA") + "\\spanly.log";
}

std::string homeDirectory() {
    return env("USERPROFILE").value_or("C:\\");
}

// MARK: Events and power

void watchSystem(SystemEvents events) {
    gEvents = std::move(events);
    win::onMessage(WM_POWERBROADCAST, [](WPARAM w, LPARAM) {
        if (w == PBT_APMSUSPEND && gEvents.displayPower) gEvents.displayPower(false);
        if (w == PBT_APMRESUMEAUTOMATIC && gEvents.displayPower) gEvents.displayPower(true);
    });
    win::onMessage(WM_DISPLAYCHANGE, [](WPARAM, LPARAM) {
        if (gEvents.screensChanged) gEvents.screensChanged();
    });
}

bool onBattery() {
    SYSTEM_POWER_STATUS s{};
    return GetSystemPowerStatus(&s) && s.ACLineStatus == 0;
}

// MARK: Login item (HKCU\...\Run)

namespace {
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
}

bool loginItemEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"Spanly", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

void setLoginItem(bool enabled) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enabled) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring value = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(key, L"Spanly", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       DWORD((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"Spanly");
    }
    RegCloseKey(key);
}

// MARK: Dialogs and files

bool ask(const std::string& title, const std::string& message, const std::string&, const std::string&) {
    return MessageBoxW(nullptr, win::wide(message).c_str(), win::wide(title).c_str(), MB_OKCANCEL | MB_ICONQUESTION) ==
           IDOK;
}

void openFile(const std::string& path) {
    ShellExecuteW(nullptr, L"open", win::wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// MARK: Helper programs

bool isExecutable(const std::string& path) {
    DWORD a = GetFileAttributesW(win::wide(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string runProcess(const std::string& path, const std::vector<std::string>& args) {
    std::wstring cmd = L"\"" + win::wide(path) + L"\"";
    for (const auto& a : args)
        cmd += L" \"" + win::wide(a) + L"\"";
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return {};
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = write;
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write);
    std::string out;
    if (ok) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(read, buf, sizeof buf, &n, nullptr) && n > 0)
            out.append(buf, n);
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(read);
    return out;
}

} // namespace spanly::platform
