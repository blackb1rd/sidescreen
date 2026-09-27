#include "platform/linux/portal.hpp"

#include "core/log.hpp"
#include "platform/platform.hpp"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <unistd.h>

namespace spanly::platform::portal {

namespace {

constexpr const char* kPortal = "org.freedesktop.portal.Desktop";
constexpr const char* kPath = "/org/freedesktop/portal/desktop";
constexpr const char* kRemoteDesktop = "org.freedesktop.portal.RemoteDesktop";
constexpr const char* kScreenCast = "org.freedesktop.portal.ScreenCast";

std::mutex gLock;
std::map<uint32_t, PortalSession*> gSessions;

/// Waits for the Response signal of one portal request, on a private main context.
class Request {
public:
    Request(GDBusConnection* bus, const std::string& token) : bus_(bus), context_(g_main_context_new()) {
        // /org/freedesktop/portal/desktop/request/SENDER/TOKEN, SENDER = our unique name without
        // the leading ':' and with '.' as '_'.
        std::string sender = g_dbus_connection_get_unique_name(bus) + 1;
        std::ranges::replace(sender, '.', '_');
        path_ = std::string(kPath) + "/request/" + sender + "/" + token;
        g_main_context_push_thread_default(context_);
        subscription_ = g_dbus_connection_signal_subscribe(bus, kPortal, "org.freedesktop.portal.Request", "Response",
                                                           path_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
                                                           onResponse, this, nullptr);
    }

    ~Request() {
        g_dbus_connection_signal_unsubscribe(bus_, subscription_);
        g_main_context_pop_thread_default(context_);
        g_main_context_unref(context_);
        if (results_) g_variant_unref(results_);
    }

    /// The results, or null if refused (response != 0) or no answer within 3 minutes.
    GVariant* wait() {
        gint64 deadline = g_get_monotonic_time() + 180 * G_TIME_SPAN_SECOND;
        while (!done_ && g_get_monotonic_time() < deadline)
            g_main_context_iteration(context_, TRUE);
        return response_ == 0 ? results_ : nullptr;
    }

private:
    static void onResponse(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* p,
                           gpointer self) {
        auto* r = static_cast<Request*>(self);
        g_variant_get(p, "(u@a{sv})", &r->response_, &r->results_);
        r->done_ = true;
    }

    GDBusConnection* bus_;
    GMainContext* context_;
    std::string path_;
    guint subscription_ = 0;
    bool done_ = false;
    guint32 response_ = 2;
    GVariant* results_ = nullptr;
};

std::string token() {
    static int n = 0;
    return "spanly" + std::to_string(getpid()) + "_" + std::to_string(++n);
}

/// Call a portal method that answers through a Request; returns its results (caller unrefs).
GVariant* request(GDBusConnection* bus, const char* iface, const char* method,
                  GVariant* (*args)(const std::string&, void*), void* data) {
    std::string t = token();
    Request r(bus, t);
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(bus, kPortal, kPath, iface, method, args(t, data), nullptr,
                                                  G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &error);
    if (!reply) {
        log("desktop portal: {}.{}: {}", iface, method, error ? error->message : "failed");
        if (error) g_error_free(error);
        return nullptr;
    }
    g_variant_unref(reply);
    GVariant* results = r.wait();
    return results ? g_variant_ref(results) : nullptr;
}

} // namespace

PortalSession* sessionFor(uint32_t displayId) {
    std::scoped_lock l(gLock);
    auto it = gSessions.find(displayId);
    return it == gSessions.end() ? nullptr : it->second;
}

void registerSession(PortalSession* s) {
    std::scoped_lock l(gLock);
    gSessions[s->node()] = s;
}

void unregisterSession(PortalSession* s) {
    std::scoped_lock l(gLock);
    std::erase_if(gSessions, [s](const auto& e) { return e.second == s; });
}

std::unique_ptr<PortalSession> PortalSession::open(bool virtualMonitor) {
    GError* error = nullptr;
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus) {
        log("desktop portal: no session bus ({})", error ? error->message : "?");
        if (error) g_error_free(error);
        return nullptr;
    }
    std::unique_ptr<PortalSession> s(new PortalSession());
    s->bus_ = bus;

    GVariant* created = request(
        bus, kRemoteDesktop, "CreateSession",
        [](const std::string& t, void*) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(t.c_str()));
            g_variant_builder_add(&b, "{sv}", "session_handle_token", g_variant_new_string(("s" + t).c_str()));
            return g_variant_new("(a{sv})", &b);
        },
        nullptr);
    const char* handle = nullptr;
    if (!created || !g_variant_lookup(created, "session_handle", "&s", &handle)) return nullptr;
    s->session_ = handle;
    g_variant_unref(created);

    struct Ctx {
        PortalSession* s;
        std::string restore;
        uint32_t sources;
    } ctx{s.get(), Store::shared().string("portalRestoreToken").value_or(""), virtualMonitor ? 4U : 1U};
    GVariant* devices = request(
        bus, kRemoteDesktop, "SelectDevices",
        [](const std::string& t, void* d) {
            auto* c = static_cast<Ctx*>(d);
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(t.c_str()));
            g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(3));        // keyboard | pointer
            g_variant_builder_add(&b, "{sv}", "persist_mode", g_variant_new_uint32(2)); // until revoked
            if (!c->restore.empty())
                g_variant_builder_add(&b, "{sv}", "restore_token", g_variant_new_string(c->restore.c_str()));
            return g_variant_new("(oa{sv})", c->s->session_.c_str(), &b);
        },
        &ctx);
    if (!devices) return nullptr;
    g_variant_unref(devices);

    GVariant* sources = request(
        bus, kScreenCast, "SelectSources",
        [](const std::string& t, void* d) {
            auto* c = static_cast<Ctx*>(d);
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(t.c_str()));
            g_variant_builder_add(&b, "{sv}", "types", g_variant_new_uint32(c->sources));
            g_variant_builder_add(&b, "{sv}", "cursor_mode", g_variant_new_uint32(2)); // embedded
            g_variant_builder_add(&b, "{sv}", "multiple", g_variant_new_boolean(FALSE));
            return g_variant_new("(oa{sv})", c->s->session_.c_str(), &b);
        },
        &ctx);
    if (!sources) {
        if (virtualMonitor) log("desktop portal: a virtual monitor needs GNOME 46+ or KDE Plasma 6 (or use Mirror)");
        return nullptr;
    }
    g_variant_unref(sources);

    GVariant* started = request(
        bus, kRemoteDesktop, "Start",
        [](const std::string& t, void* d) {
            auto* c = static_cast<Ctx*>(d);
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(t.c_str()));
            return g_variant_new("(osa{sv})", c->s->session_.c_str(), "", &b);
        },
        &ctx);
    if (!started) {
        log("desktop portal: screen sharing was not allowed");
        return nullptr;
    }
    if (const char* restore = nullptr; g_variant_lookup(started, "restore_token", "&s", &restore))
        Store::shared().set("portalRestoreToken", std::string(restore));
    GVariant* streams = g_variant_lookup_value(started, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    if (!streams || g_variant_n_children(streams) == 0) {
        log("desktop portal: the desktop didn't share a screen");
        return nullptr;
    }
    GVariant* props = nullptr;
    g_variant_get_child(streams, 0, "(u@a{sv})", &s->node_, &props);
    g_variant_lookup(props, "size", "(ii)", &s->width_, &s->height_);
    g_variant_unref(props);
    g_variant_unref(streams);
    g_variant_unref(started);

    GUnixFDList* fds = nullptr;
    GVariantBuilder none;
    g_variant_builder_init(&none, G_VARIANT_TYPE_VARDICT);
    GVariant* remote = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, kPortal, kPath, kScreenCast, "OpenPipeWireRemote", g_variant_new("(oa{sv})", s->session_.c_str(), &none),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fds, nullptr, &error);
    if (!remote) {
        log("desktop portal: no PipeWire stream ({})", error ? error->message : "?");
        if (error) g_error_free(error);
        return nullptr;
    }
    gint32 index = 0;
    g_variant_get(remote, "(h)", &index);
    s->fd_ = g_unix_fd_list_get(fds, index, nullptr);
    g_variant_unref(remote);
    g_object_unref(fds);
    log("desktop portal: sharing PipeWire node {}, {}x{}", s->node_, s->width_, s->height_);
    registerSession(s.get());
    return s;
}

PortalSession::~PortalSession() {
    unregisterSession(this);
    if (fd_ >= 0) close(fd_);
    if (bus_ && !session_.empty())
        g_dbus_connection_call(bus_, kPortal, session_.c_str(), "org.freedesktop.portal.Session", "Close", nullptr,
                               nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
    if (bus_) g_object_unref(bus_);
}

namespace {

void notify(GDBusConnection* bus, const char* method, GVariant* args) {
    g_dbus_connection_call(bus, kPortal, kPath, kRemoteDesktop, method, args, nullptr, G_DBUS_CALL_FLAGS_NONE, -1,
                           nullptr, nullptr, nullptr);
}

GVariant* noOptions() {
    return g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0);
}

} // namespace

void PortalSession::pointerTo(double x, double y) const {
    notify(bus_, "NotifyPointerMotionAbsolute",
           g_variant_new("(o@a{sv}udd)", session_.c_str(), noOptions(), node_, x, y));
}

void PortalSession::button(int code, bool pressed) const {
    notify(bus_, "NotifyPointerButton",
           g_variant_new("(o@a{sv}iu)", session_.c_str(), noOptions(), code, pressed ? 1U : 0U));
}

void PortalSession::axis(double dx, double dy) const {
    notify(bus_, "NotifyPointerAxis", g_variant_new("(o@a{sv}dd)", session_.c_str(), noOptions(), dx, dy));
}

void PortalSession::key(int code, bool pressed) const {
    notify(bus_, "NotifyKeyboardKeycode",
           g_variant_new("(o@a{sv}iu)", session_.c_str(), noOptions(), code, pressed ? 1U : 0U));
}

} // namespace spanly::platform::portal
