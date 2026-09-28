// The menu bar item on macOS (NSStatusItem + NSMenu), built from the app's MenuItem model each
// time it opens; and the modal alert.
#include "platform/mac/objc.hpp"
#include "platform/platform.hpp"

#include <CoreGraphics/CoreGraphics.h>

namespace spanly::platform {

using namespace mac;

namespace {

class MacTray : public Tray {
public:
    explicit MacTray(std::function<std::vector<MenuItem>()> build) : build_(std::move(build)) {
        Pool pool;
        target_ = Ref(instance(targetClass(), this));
        item_ = Ref(retain(send(send(cls("NSStatusBar"), "systemStatusBar"), "statusItemWithLength:", -2.0)));
        menu_ = Ref(make("NSMenu"));
        send<void>(menu_.get(), "setAutoenablesItems:", NO);
        send<void>(menu_.get(), "setDelegate:", target_.get());
        send<void>(item_.get(), "setMenu:", menu_.get());
        MacTray::setState(TrayState::Idle); // no subclass exists yet while constructing
    }

    ~MacTray() override { send<void>(send(cls("NSStatusBar"), "systemStatusBar"), "removeStatusItem:", item_.get()); }

    void setState(TrayState state) override {
        if (state == state_ && hasIcon_) return;
        Pool pool;
        state_ = state;
        hasIcon_ = true;
        const char* symbol = state == TrayState::Warning     ? "exclamationmark.triangle"
                             : state == TrayState::Streaming ? "rectangle.fill.on.rectangle.fill"
                                                             : "rectangle.on.rectangle";
        Obj image = send(cls("NSImage"), "imageWithSystemSymbolName:accessibilityDescription:", nsString(symbol),
                         nsString("Spanly"));
        send<void>(image, "setTemplate:", YES);
        send<void>(send(item_.get(), "button"), "setImage:", image);
    }

private:
    void rebuild() {
        Pool pool;
        send<void>(menu_.get(), "removeAllItems");
        actions_.clear();
        fill(menu_.get(), build_());
    }

    void fill(Obj menu, const std::vector<MenuItem>& items) {
        for (const auto& it : items) {
            if (it.kind == MenuItem::Kind::Separator) {
                send<void>(menu, "addItem:", send(cls("NSMenuItem"), "separatorItem"));
                continue;
            }
            bool action = it.kind == MenuItem::Kind::Action;
            Obj item =
                send(send(send(cls("NSMenuItem"), "alloc"), "initWithTitle:action:keyEquivalent:", nsString(it.title),
                          action ? sel("fire:") : nullptr, nsString(it.key)),
                     "autorelease");
            if (action) {
                send<void>(item, "setTarget:", target_.get());
                send<void>(item, "setTag:", long(actions_.size()));
                actions_.push_back(it.action);
            }
            send<void>(item, "setState:", long(it.checked ? 1 : 0));
            send<void>(item, "setEnabled:", BOOL(action && it.enabled));
            if (it.bold) {
                Obj font = send(cls("NSFont"), "boldSystemFontOfSize:", 13.0);
                Obj attrs = send(cls("NSDictionary"), "dictionaryWithObject:forKey:", font, nsString("NSFont"));
                Obj title = send(send(send(cls("NSAttributedString"), "alloc"),
                                      "initWithString:attributes:", nsString(it.title), attrs),
                                 "autorelease");
                send<void>(item, "setAttributedTitle:", title);
            }
            if (it.kind == MenuItem::Kind::Submenu) {
                Obj sub = send(make("NSMenu"), "autorelease");
                send<void>(sub, "setAutoenablesItems:", NO);
                fill(sub, it.children);
                send<void>(item, "setSubmenu:", sub);
                send<void>(item, "setEnabled:", YES);
            }
            send<void>(menu, "addItem:", item);
        }
    }

    static Class targetClass() {
        static Class c = [] {
            return ClassBuilder("SpanlyMenuTarget")
                .protocol("NSMenuDelegate")
                .method(
                    "menuNeedsUpdate:", +[](Obj self, SEL, Obj) { owner<MacTray>(self)->rebuild(); }, "v@:@")
                .method(
                    "fire:",
                    +[](Obj self, SEL, Obj sender) {
                        auto* t = owner<MacTray>(self);
                        auto tag = size_t(send<long>(sender, "tag"));
                        if (tag < t->actions_.size() && t->actions_[tag]) {
                            auto action = t->actions_[tag]; // the menu may be rebuilt by the action
                            action();
                        }
                    },
                    "v@:@")
                .build();
        }();
        return c;
    }

    std::function<std::vector<MenuItem>()> build_;
    std::vector<std::function<void()>> actions_;
    Ref target_, item_, menu_;
    TrayState state_ = TrayState::Idle;
    bool hasIcon_ = false;
};

} // namespace

std::unique_ptr<Tray> Tray::create(std::function<std::vector<MenuItem>()> build) {
    return std::make_unique<MacTray>(std::move(build));
}

bool ask(const std::string& title, const std::string& message, const std::string& ok, const std::string& cancel) {
    Pool pool;
    send<void>(send(cls("NSApplication"), "sharedApplication"), "activateIgnoringOtherApps:", YES);
    Ref alert(make("NSAlert"));
    send<void>(alert.get(), "setMessageText:", nsString(title));
    send<void>(alert.get(), "setInformativeText:", nsString(message));
    send(alert.get(), "addButtonWithTitle:", nsString(ok));
    send(alert.get(), "addButtonWithTitle:", nsString(cancel));
    return send<long>(alert.get(), "runModal") == 1000; // NSAlertFirstButtonReturn
}

void openFile(const std::string& path) {
    Pool pool;
    Obj url = send(cls("NSURL"), "fileURLWithPath:", nsString(path));
    send<BOOL>(send(cls("NSWorkspace"), "sharedWorkspace"), "openURL:", url);
}

} // namespace spanly::platform
