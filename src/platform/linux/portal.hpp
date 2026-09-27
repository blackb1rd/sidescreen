#pragma once
// xdg-desktop-portal: one RemoteDesktop session that also casts the screen (a new virtual
// monitor in extend mode, or the main one in mirror mode). The user approves it once in the
// desktop's dialog; the restore token saved in the settings skips the dialog next time. Works on
// GNOME (46+) and KDE Plasma (6+), Wayland and X11, without root.

#include <cstdint>
#include <memory>
#include <string>

struct _GDBusConnection;

namespace spanly::platform::portal {

class PortalSession {
public:
    /// Blocks until the desktop has answered (the first time, the user approves in a dialog).
    /// Null if the desktop refused or has no portal.
    static std::unique_ptr<PortalSession> open(bool virtualMonitor);
    ~PortalSession();

    int pipeWireFd() const { return fd_; } // owned by the session; dup() it to keep it
    uint32_t node() const { return node_; }
    int width() const { return width_; }
    int height() const { return height_; }

    // Input, positions in the stream's logical pixels. Fire and forget.
    void pointerTo(double x, double y) const;
    void button(int evdevCode, bool pressed) const;
    void axis(double dx, double dy) const;
    void key(int evdevCode, bool pressed) const;

private:
    PortalSession() = default;
    _GDBusConnection* bus_ = nullptr;
    std::string session_;
    int fd_ = -1;
    uint32_t node_ = 0;
    int width_ = 1920, height_ = 1080;
};

/// The session that made display `id` (its PipeWire node), if any.
PortalSession* sessionFor(uint32_t displayId);
void registerSession(PortalSession* s);
void unregisterSession(PortalSession* s);

} // namespace spanly::platform::portal
