// System events on macOS (sleep/wake, display changes, quitting), battery power and the login item.
#include "platform/mac/objc.hpp"
#include "platform/platform.hpp"

#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>

namespace spanly::platform {

using namespace mac;

namespace {

// NOLINTNEXTLINE(performance-unnecessary-value-param): the block keeps a copy (a reference would dangle)
void observe(Obj center, const char* name, std::function<void()> fn) {
    Obj queue = send(cls("NSOperationQueue"), "mainQueue");
    void (^block)(Obj) = ^(Obj) {
      fn();
    };
    send(center, "addObserverForName:object:queue:usingBlock:", nsString(name), nullptr, queue, blockObject(block));
}

} // namespace

void watchSystem(SystemEvents events) {
    Pool pool;
    static SystemEvents e; // lives as long as the app
    e = std::move(events);
    Obj workspace = send(send(cls("NSWorkspace"), "sharedWorkspace"), "notificationCenter");
    for (const char* n : {"NSWorkspaceScreensDidSleepNotification", "NSWorkspaceWillSleepNotification"})
        observe(workspace, n, [] {
            if (e.displayPower) e.displayPower(false);
        });
    for (const char* n : {"NSWorkspaceScreensDidWakeNotification", "NSWorkspaceDidWakeNotification"})
        observe(workspace, n, [] {
            if (e.displayPower) e.displayPower(true);
        });
    Obj center = send(cls("NSNotificationCenter"), "defaultCenter");
    observe(center, "NSApplicationDidChangeScreenParametersNotification", [] {
        if (e.screensChanged) e.screensChanged();
    });
    observe(center, "NSApplicationWillTerminateNotification", [] {
        if (e.willQuit) e.willQuit();
    });
}

/// spanly:// links arrive as a "get URL" Apple event.
void onOpenUrl(std::function<void(const std::string&)> handler) {
    static std::function<void(const std::string&)> h;
    h = std::move(handler);
    static Class c = ClassBuilder("SpanlyUrlHandler")
                         .method(
                             "handleURL:withReplyEvent:",
                             +[](Obj, SEL, Obj event, Obj) {
                                 Obj param = send(event, "paramDescriptorForKeyword:", uint32_t('----'));
                                 if (h && param) h(toString(send(param, "stringValue")));
                             },
                             "v@:@@")
                         .build();
    static Obj target = send(reinterpret_cast<Obj>(class_createInstance(c, 0)), "init");
    Obj manager = send(cls("NSAppleEventManager"), "sharedAppleEventManager");
    send<void>(manager, "setEventHandler:andSelector:forEventClass:andEventID:", target,
               sel("handleURL:withReplyEvent:"), uint32_t('GURL'), uint32_t('GURL'));
}

bool onBattery() {
    CFTypeRef info = IOPSCopyPowerSourcesInfo();
    if (!info) return false;
    CFStringRef type = IOPSGetProvidingPowerSourceType(info);
    bool battery = type && CFStringCompare(type, CFSTR(kIOPSBatteryPowerValue), 0) == kCFCompareEqualTo;
    CFRelease(info);
    return battery;
}

// The login item (SMAppService, macOS 13+): status 1 = enabled, 2 = waiting for approval.
bool loginItemEnabled() {
    Pool pool;
    long status = send<long>(send(cls("SMAppService"), "mainAppService"), "status");
    return status == 1 || status == 2;
}

void setLoginItem(bool enabled) {
    Pool pool;
    Obj service = send(cls("SMAppService"), "mainAppService");
    Obj error = nullptr;
    send<BOOL>(service, enabled ? "registerAndReturnError:" : "unregisterAndReturnError:", &error);
}

} // namespace spanly::platform
