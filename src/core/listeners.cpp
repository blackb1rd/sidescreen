#include "core/listeners.hpp"

#include "core/log.hpp"

#include <algorithm>

namespace spanly {

TcpListener::TcpListener(uint16_t port, Link::Kind kind, std::function<Bytes()> secret)
    : port_(port), kind_(kind), secret_(std::move(secret)) {}

TcpListener::~TcpListener() {
    try {
        setEnabled(false);
    } catch (...) { // NOLINT(bugprone-empty-catch): nothing more to do while shutting down
    }
}

bool TcpListener::setEnabled(bool enabled) {
    if (enabled == running_) return true;
    if (enabled) {
        socket_ = net::listenTcp(port_, kind_ == Link::Kind::Adb);
        if (!socket_.valid()) {
            log("could not listen on port {} (is Spanly already running?)", port_);
            return false;
        }
        running_ = true;
        thread_ = std::thread([this] { acceptLoop(); });
        return true;
    }
    running_ = false;
    if (thread_.joinable()) thread_.join();
    socket_.close();
    std::vector<std::weak_ptr<TcpLink>> links;
    {
        std::scoped_lock l(m_);
        links.swap(links_);
    }
    for (auto& w : links) {
        if (auto link = w.lock()) link->close("listener stopped");
    }
    return true;
}

void TcpListener::acceptLoop() {
    while (running_) {
        std::string peer;
        net::Socket s = net::accept(socket_, 500, &peer);
        if (!s.valid()) continue;
        auto link = std::make_shared<TcpLink>(std::move(s), kind_,
                                              secret_ ? std::optional<Bytes>(secret_()) : std::nullopt, peer);
        std::vector<std::shared_ptr<TcpLink>> replaced;
        {
            std::scoped_lock l(m_);
            std::erase_if(links_, [](auto& w) { return w.expired(); });
            if (kind_ == Link::Kind::Adb) { // one tablet through adb: a new connection replaces the old
                for (auto& w : links_) {
                    if (auto old = w.lock()) replaced.push_back(old);
                }
                links_.clear();
            }
            links_.push_back(link);
        }
        for (auto& old : replaced)
            old->close("replaced by a new connection");
        if (onConnection) onConnection(link);
        link->start();
    }
}

} // namespace spanly
