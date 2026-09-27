#pragma once
// Shared by the Windows backend: the hidden window whose message loop is the app's main thread,
// and UTF-8 <-> UTF-16 conversion.

#include <windows.h>

#include <functional>
#include <string>

namespace spanly::platform::win {

/// The app's hidden top-level window (it receives power and display-change broadcasts).
HWND window();
/// Handle `message` sent to that window (main thread).
void onMessage(UINT message, std::function<void(WPARAM, LPARAM)> handler);

constexpr UINT kRunMessage = WM_APP + 1;
constexpr UINT kTrayMessage = WM_APP + 2;

std::wstring wide(const std::string& s);
std::string utf8(const std::wstring& s);

} // namespace spanly::platform::win

namespace spanly::platform {
/// The monitor's device name (\\.\DISPLAY2) for a display id, or empty.
std::wstring displayDevice(uint32_t id);
} // namespace spanly::platform
