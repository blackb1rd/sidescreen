// The macOS run loop, main-thread scheduling, permissions and settings.
#include "platform/mac/objc.hpp"
#include "platform/platform.hpp"

#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <dispatch/dispatch.h>

#include <cstdlib>

namespace spanly::platform {

using namespace mac;

// MARK: Main thread

namespace {

void runFunction(void* context) {
    std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(context));
    Pool pool;
    (*fn)();
}

} // namespace

int runApp(const std::function<void()>& start) {
    Pool pool;
    Obj app = send(cls("NSApplication"), "sharedApplication");
    send<void>(app, "setActivationPolicy:", 1L); // accessory: a menu bar app, no Dock icon
    runOnMain(start);
    send<void>(app, "run");
    return 0;
}

void quit() {
    runOnMain([] { send<void>(send(cls("NSApplication"), "sharedApplication"), "terminate:", nullptr); });
}

void runOnMain(std::function<void()> fn) {
    dispatch_async_f(dispatch_get_main_queue(), new std::function<void()>(std::move(fn)), runFunction);
}

void runAfter(double seconds, std::function<void()> fn) {
    dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, int64_t(seconds * 1e9)), dispatch_get_main_queue(),
                     new std::function<void()>(std::move(fn)), runFunction);
}

// MARK: Permissions

bool screenRecordingAllowed() {
    return CGPreflightScreenCaptureAccess();
}

void requestScreenRecording() {
    if (!CGRequestScreenCaptureAccess()) {
        Pool pool;
        Obj url = send(cls("NSURL"), "URLWithString:",
                       nsString("x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture"));
        send<BOOL>(send(cls("NSWorkspace"), "sharedWorkspace"), "openURL:", url);
    }
}

bool accessibilityAllowed() {
    return AXIsProcessTrusted();
}

void requestAccessibility() {
    const void* keys[] = {kAXTrustedCheckOptionPrompt};
    const void* values[] = {kCFBooleanTrue};
    CFDictionaryRef opts =
        CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    AXIsProcessTrustedWithOptions(opts);
    CFRelease(opts);
}

// MARK: Names and paths

std::string computerName() {
    CFStringRef name = SCDynamicStoreCopyComputerName(nullptr, nullptr);
    if (!name) return "Mac";
    char buf[256] = {};
    CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8);
    CFRelease(name);
    return buf;
}

std::string logFilePath() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/Library/Logs/Spanly.log";
}

// MARK: Settings

namespace {

/// The app's preferences domain (the same one earlier versions of the app used).
class MacStore : public Store {
public:
    std::optional<std::string> string(const std::string& key) override {
        CFTypeRef v = copy(key);
        std::optional<std::string> out;
        if (v && CFGetTypeID(v) == CFStringGetTypeID()) {
            char buf[1024] = {};
            if (CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf, kCFStringEncodingUTF8)) out = buf;
        }
        if (v) CFRelease(v);
        return out;
    }

    std::optional<Bytes> data(const std::string& key) override {
        CFTypeRef v = copy(key);
        std::optional<Bytes> out;
        if (v && CFGetTypeID(v) == CFDataGetTypeID()) {
            auto d = static_cast<CFDataRef>(v);
            out = Bytes(CFDataGetBytePtr(d), CFDataGetBytePtr(d) + CFDataGetLength(d));
        }
        if (v) CFRelease(v);
        return out;
    }

    std::optional<bool> boolean(const std::string& key) override {
        CFTypeRef v = copy(key);
        std::optional<bool> out;
        if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) out = CFBooleanGetValue(static_cast<CFBooleanRef>(v));
        if (v && CFGetTypeID(v) == CFNumberGetTypeID()) {
            int n = 0;
            CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberIntType, &n);
            out = n != 0;
        }
        if (v) CFRelease(v);
        return out;
    }

    void set(const std::string& key, const std::string& value) override {
        CFStringRef v = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(value.data()),
                                                CFIndex(value.size()), kCFStringEncodingUTF8, false);
        put(key, v);
        CFRelease(v);
    }

    void set(const std::string& key, const Bytes& value) override {
        CFDataRef v = CFDataCreate(nullptr, value.data(), CFIndex(value.size()));
        put(key, v);
        CFRelease(v);
    }

    void set(const std::string& key, bool value) override { put(key, value ? kCFBooleanTrue : kCFBooleanFalse); }

    void remove(const std::string& key) override { put(key, nullptr); }

private:
    static CFStringRef domain() { return CFSTR("com.caigenix.spanly"); }

    static CFStringRef cfKey(const std::string& key) {
        return CFStringCreateWithCString(nullptr, key.c_str(), kCFStringEncodingUTF8);
    }

    static CFTypeRef copy(const std::string& key) {
        CFStringRef k = cfKey(key);
        CFTypeRef v = CFPreferencesCopyAppValue(k, domain());
        CFRelease(k);
        return v;
    }

    static void put(const std::string& key, CFTypeRef value) {
        CFStringRef k = cfKey(key);
        CFPreferencesSetAppValue(k, value, domain());
        CFPreferencesAppSynchronize(domain());
        CFRelease(k);
    }
};

} // namespace

Store& Store::shared() {
    static MacStore store;
    return store;
}

} // namespace spanly::platform
