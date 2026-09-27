#include "app/settings.hpp"

#include "core/crypto.hpp"
#include "platform/platform.hpp"

namespace spanly {

namespace {
platform::Store& store() {
    return platform::Store::shared();
}
} // namespace

Mode Settings::mode() const {
    return store().string("mode") == "mirror" ? Mode::Mirror : Mode::Extend;
}
void Settings::setMode(Mode m) {
    store().set("mode", std::string(m == Mode::Mirror ? "mirror" : "extend"));
}

Quality Settings::quality() const {
    auto q = store().string("quality");
    return q == "retina" ? Quality::Retina : q == "standard" ? Quality::Standard : Quality::Auto;
}
void Settings::setQuality(Quality q) {
    store().set("quality", std::string(q == Quality::Retina ? "retina" : q == Quality::Standard ? "standard" : "auto"));
}

std::string Settings::position() const {
    return store().string("position").value_or("right");
}
void Settings::setPosition(const std::string& p) {
    store().set("position", p);
}

bool Settings::wifi() const {
    return store().boolean("wifi").value_or(true);
}
void Settings::setWifi(bool on) {
    store().set("wifi", on);
}

Bytes Settings::wifiSecret() {
    if (auto s = store().data("wifiSecret"); s && s->size() == 32) return *s;
    return resetWifiSecret();
}
Bytes Settings::resetWifiSecret() {
    Bytes s = crypto::random(32);
    store().set("wifiSecret", s);
    return s;
}

Sound Settings::sound() const {
    auto s = store().string("sound");
    if (s == "tablet") return Sound::Tablet;
    if (s == "both") return Sound::Both;
    if (s == "mac") return Sound::Mac;
    return store().boolean("audio").value_or(false) ? Sound::Both : Sound::Mac; // earlier on/off setting
}
void Settings::setSound(Sound s) {
    store().set("sound", std::string(s == Sound::Tablet ? "tablet" : s == Sound::Both ? "both" : "mac"));
}

bool Settings::tabletMicrophone() const {
    return store().boolean("tabletMicrophone").value_or(false);
}
void Settings::setTabletMicrophone(bool on) {
    store().set("tabletMicrophone", on);
}

bool Settings::restoreCursor() const {
    return store().boolean("restoreCursor").value_or(true);
}
void Settings::setRestoreCursor(bool on) {
    store().set("restoreCursor", on);
}

std::optional<std::string> Settings::deviceSerial() const {
    return store().string("deviceSerial");
}
std::optional<std::string> Settings::deviceName() const {
    return store().string("deviceName");
}
void Settings::setDevice(const std::string& serial, const std::string& name) {
    store().set("deviceSerial", serial);
    store().set("deviceName", name);
}

} // namespace spanly
