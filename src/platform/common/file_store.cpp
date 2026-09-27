// Settings in a small text file (Windows and Linux): one "key=value" per line; data values are
// hex. Kept in memory and written back on every change.
#include "core/log.hpp"
#include "platform/platform.hpp"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace spanly::platform {

namespace {

class FileStore : public Store {
public:
    FileStore() : path_(configDirectory() + "/settings") {
        std::ifstream in(path_);
        for (std::string line; std::getline(in, line);) {
            auto eq = line.find('=');
            if (eq != std::string::npos) values_[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }

    std::optional<std::string> string(const std::string& key) override { return get(key); }

    std::optional<Bytes> data(const std::string& key) override {
        auto v = get(key);
        if (!v || v->size() % 2) return std::nullopt;
        Bytes b;
        for (size_t i = 0; i < v->size(); i += 2)
            b.push_back(uint8_t(std::stoi(v->substr(i, 2), nullptr, 16)));
        return b;
    }

    std::optional<bool> boolean(const std::string& key) override {
        auto v = get(key);
        if (!v) return std::nullopt;
        return *v == "1" || *v == "true";
    }

    void set(const std::string& key, const std::string& value) override { put(key, value); }

    void set(const std::string& key, const Bytes& value) override {
        static const char* hex = "0123456789abcdef";
        std::string s;
        for (uint8_t b : value) {
            s += hex[b >> 4U];
            s += hex[b & 15U];
        }
        put(key, s);
    }

    void set(const std::string& key, bool value) override { put(key, value ? "1" : "0"); }

    void remove(const std::string& key) override {
        std::scoped_lock l(m_);
        values_.erase(key);
        save();
    }

private:
    std::optional<std::string> get(const std::string& key) {
        std::scoped_lock l(m_);
        auto it = values_.find(key);
        return it == values_.end() ? std::nullopt : std::optional(it->second);
    }

    void put(const std::string& key, const std::string& value) {
        std::scoped_lock l(m_);
        values_[key] = value;
        save();
    }

    void save() {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path(), ec);
        std::ofstream out(path_, std::ios::trunc);
        for (const auto& [k, v] : values_)
            out << k << '=' << v << '\n';
        if (!out) log("could not save settings to {}", path_);
    }

    std::string path_;
    std::mutex m_;
    std::map<std::string, std::string> values_;
};

} // namespace

Store& Store::shared() {
    static FileStore store;
    return store;
}

} // namespace spanly::platform
