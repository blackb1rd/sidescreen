// The notification-area (tray) icon on Windows, with the app's menu built each time it opens.
#include "platform/platform.hpp"
#include "platform/windows/win.hpp"

#include <shellapi.h>

namespace spanly::platform {

namespace {

class WinTray : public Tray {
public:
    explicit WinTray(std::function<std::vector<MenuItem>()> build) : build_(std::move(build)) {
        icon_.cbSize = sizeof icon_;
        icon_.hWnd = win::window();
        icon_.uID = 1;
        icon_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        icon_.uCallbackMessage = win::kTrayMessage;
        icon_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(icon_.szTip, L"Spanly");
        Shell_NotifyIconW(NIM_ADD, &icon_);
        win::onMessage(win::kTrayMessage, [this](WPARAM, LPARAM l) {
            if (LOWORD(l) == WM_LBUTTONUP || LOWORD(l) == WM_RBUTTONUP) show();
        });
    }

    ~WinTray() override { Shell_NotifyIconW(NIM_DELETE, &icon_); }

    void setState(TrayState state) override {
        icon_.hIcon = LoadIconW(nullptr, state == TrayState::Warning ? IDI_WARNING : IDI_APPLICATION);
        const wchar_t* tip = state == TrayState::Streaming ? L"Spanly: streaming" : L"Spanly";
        wcscpy_s(icon_.szTip, tip);
        Shell_NotifyIconW(NIM_MODIFY, &icon_);
    }

private:
    void show() {
        actions_.clear();
        HMENU menu = CreatePopupMenu();
        fill(menu, build_());
        POINT p;
        GetCursorPos(&p);
        SetForegroundWindow(win::window()); // so the menu closes when clicking elsewhere
        UINT id = UINT(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, win::window(), nullptr));
        PostMessageW(win::window(), WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (id > 0 && id <= actions_.size() && actions_[id - 1]) {
            auto action = actions_[id - 1];
            action();
        }
    }

    void fill(HMENU menu, const std::vector<MenuItem>& items) {
        for (const auto& it : items) {
            std::wstring title = win::wide(it.title);
            switch (it.kind) {
            case MenuItem::Kind::Separator: AppendMenuW(menu, MF_SEPARATOR, 0, nullptr); break;
            case MenuItem::Kind::Label: AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title.c_str()); break;
            case MenuItem::Kind::Submenu: {
                HMENU sub = CreatePopupMenu();
                fill(sub, it.children);
                AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), title.c_str());
                break;
            }
            case MenuItem::Kind::Action:
                actions_.push_back(it.action);
                AppendMenuW(menu, MF_STRING | (it.checked ? MF_CHECKED : 0U) | (it.enabled ? 0U : MF_GRAYED),
                            actions_.size(), title.c_str());
                break;
            }
        }
    }

    std::function<std::vector<MenuItem>()> build_;
    std::vector<std::function<void()>> actions_;
    NOTIFYICONDATAW icon_{};
};

} // namespace

std::unique_ptr<Tray> Tray::create(std::function<std::vector<MenuItem>()> build) {
    return std::make_unique<WinTray>(std::move(build));
}

} // namespace spanly::platform
