#pragma once
// Calling Apple's Objective-C APIs (AppKit, ScreenCaptureKit, the virtual display) from plain
// C++ through the Objective-C runtime, the way Apple's metal-cpp does: send<R>(object,
// "selector:", args...) is [object selector:args]. Completion handlers are Clang blocks.

#include <objc/message.h>
#include <objc/runtime.h>

#include <CoreFoundation/CoreFoundation.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void* pool);

namespace spanly::mac {

/// An Objective-C object (the runtime's `id`; named so it can't clash with C++ members called id()).
using Obj = ::id;

inline SEL sel(const char* name) {
    return sel_registerName(name);
}

inline Obj cls(const char* name) {
    return reinterpret_cast<Obj>(objc_getClass(name));
}

/// [target selector:args...] returning R (Obj by default).
template <class R = Obj, class... Args>
R send(Obj target, const char* selector, Args... args) {
    using Fn = R (*)(Obj, SEL, Args...);
#if defined(__x86_64__)
    // Large structs come back through objc_msgSend_stret on Intel.
    if constexpr (std::is_class_v<R>) {
        if constexpr (sizeof(R) > 16) return reinterpret_cast<Fn>(objc_msgSend_stret)(target, sel(selector), args...);
    }
#endif
    return reinterpret_cast<Fn>(objc_msgSend)(target, sel(selector), args...);
}

/// [[Class alloc] init]: an owned (+1) object.
inline Obj make(const char* className) {
    return send(send(cls(className), "alloc"), "init");
}

inline void release(Obj object) {
    if (object) send<void>(object, "release");
}

inline Obj retain(Obj object) {
    return object ? send(object, "retain") : nullptr;
}

/// An autoreleased NSString.
inline Obj nsString(std::string_view s) {
    Obj o = send(send(cls("NSString"), "alloc"), "initWithBytes:length:encoding:", static_cast<const void*>(s.data()),
                 static_cast<unsigned long>(s.size()), 4UL /* NSUTF8StringEncoding */);
    return o ? send(o, "autorelease") : nullptr;
}

inline std::string toString(Obj nsString) {
    if (!nsString) return {};
    const char* s = send<const char*>(nsString, "UTF8String");
    return s ? s : "";
}

/// A block (^{...}) passed where an Objective-C object is expected.
template <class Block>
Obj blockObject(Block block) {
    return reinterpret_cast<Obj>(block);
}

/// Autorelease pool for the current scope (needed on threads Cocoa doesn't manage).
class Pool {
public:
    Pool() : pool_(objc_autoreleasePoolPush()) {}
    ~Pool() { objc_autoreleasePoolPop(pool_); }
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

private:
    void* pool_;
};

/// An owned Objective-C object, released when this goes away.
class Ref {
public:
    Ref() = default;
    explicit Ref(Obj owned) : o_(owned) {}
    Ref(const Ref& other) : o_(retain(other.o_)) {}
    Ref(Ref&& other) noexcept : o_(other.o_) { other.o_ = nullptr; }
    Ref& operator=(Ref other) noexcept {
        std::swap(o_, other.o_);
        return *this;
    }
    ~Ref() { release(o_); }
    Obj get() const { return o_; }
    explicit operator bool() const { return o_ != nullptr; }

private:
    Obj o_ = nullptr;
};

/// A new Objective-C class (e.g. a delegate) whose instances point back to a C++ object.
class ClassBuilder {
public:
    ClassBuilder(const char* name, const char* superclass = "NSObject")
        : cls_(objc_allocateClassPair(objc_getClass(superclass), name, 0)) {
        class_addIvar(cls_, "cpp", sizeof(void*), alignof(void*), "^v");
    }
    template <class Fn>
    ClassBuilder& method(const char* selector, Fn fn, const char* types) {
        class_addMethod(cls_, sel(selector), reinterpret_cast<IMP>(fn), types);
        return *this;
    }
    ClassBuilder& protocol(const char* name) {
        if (Protocol* p = objc_getProtocol(name)) class_addProtocol(cls_, p);
        return *this;
    }
    Class build() {
        objc_registerClassPair(cls_);
        return cls_;
    }

private:
    Class cls_;
};

/// The C++ object behind an instance of a ClassBuilder class.
template <class T>
T* owner(Obj self) {
    Ivar ivar = class_getInstanceVariable(object_getClass(self), "cpp");
    return *reinterpret_cast<T**>(reinterpret_cast<char*>(self) + ivar_getOffset(ivar));
}

/// A new instance of a ClassBuilder class pointing to `cppObject`; initialised with -init unless
/// `init` is false (the caller then sends its own initializer, e.g. -initWithFrame:).
template <class T>
Obj instance(Class cls, T* cppObject, bool init = true) {
    auto o = reinterpret_cast<Obj>(class_createInstance(cls, 0));
    Ivar ivar = class_getInstanceVariable(cls, "cpp");
    *reinterpret_cast<T**>(reinterpret_cast<char*>(o) + ivar_getOffset(ivar)) = cppObject;
    return init ? send(o, "init") : o;
}

/// [super selector:args...] from a method of a ClassBuilder class.
template <class R = void, class... Args>
R sendSuper(Obj self, const char* selector, Args... args) {
    objc_super sup{self, class_getSuperclass(object_getClass(self))};
    using Fn = R (*)(objc_super*, SEL, Args...);
    return reinterpret_cast<Fn>(objc_msgSendSuper)(&sup, sel(selector), args...);
}

} // namespace spanly::mac
