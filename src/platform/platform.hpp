#pragma once
// What each operating system provides to the shared core: the main thread, displays (and a
// virtual one sized to the tablet), screen capture, hardware video encoding, input injection,
// settings storage and permissions. src/platform/<os>/ implements it.

#include "core/flow.hpp"
#include "core/protocol.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace spanly::platform {

using DisplayId = uint32_t;

struct Point {
    double x = 0, y = 0;
};

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(Point p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
};

/// What this platform can do beyond streaming (the menu leaves out the rest).
enum class Feature { Sound, Microphone, TabletWindow, Tray };
bool supports(Feature f);

// MARK: Main thread

/// Run the event loop; `start` runs on the main thread once it is up. Returns when quit() is called.
int runApp(const std::function<void()>& start);
void quit();
void runOnMain(std::function<void()> fn);
void runAfter(double seconds, std::function<void()> fn); // on the main thread

// MARK: Displays

DisplayId mainDisplay();
/// The display's current mode in pixels.
std::optional<std::pair<int, int>> displayPixels(DisplayId id);
Rect displayBounds(DisplayId id); // in points, global coordinates
std::optional<DisplayId> displayNamed(const std::string& fragment);
Point cursorLocation();

/// A display that exists only while Spanly needs it, sized to the tablet. A fixed serial
/// number per tablet lets the OS remember where it was arranged.
class VirtualDisplay {
public:
    static std::unique_ptr<VirtualDisplay> create(int tabletW, int tabletH, int dpi, bool hiDPI, uint32_t serial);
    virtual ~VirtualDisplay() = default;
    virtual DisplayId id() const = 0;
    virtual int tabletW() const = 0;
    virtual int tabletH() const = 0;
    virtual bool hiDPI() const = 0;
    /// Reshape in place (the tablet turned), keeping its windows.
    virtual bool resize(int tabletW, int tabletH) = 0;
    /// Pick the mode matching the tablet (the OS may choose another generated one).
    virtual void selectMode() = 0;
    /// Move it next to the main display, beyond `others` (the other tablets' displays).
    virtual void place(const std::string& position, const std::vector<DisplayId>& others) = 0;
};

// MARK: Capture and encoding

/// A captured picture: a GPU image owned by the platform (CVPixelBuffer on macOS).
struct Frame {
    std::shared_ptr<void> image;
    int width = 0, height = 0;
    Clock::time_point captured;
};

class Capture {
public:
    static std::unique_ptr<Capture> create();
    virtual ~Capture() = default;

    std::function<void(const Frame&)> onFrame; // capture thread
    std::function<void(const Bytes&)> onAudio; // 48 kHz 16-bit stereo PCM, audio thread
    /// Capture ended by itself: `byUser` when the user stopped it (macOS's screen-sharing menu),
    /// otherwise the system did (e.g. the display went away).
    std::function<void(bool byUser)> onStop;

    /// Start capturing a display at the given size; `done` gets "" or an error (any thread).
    virtual void start(DisplayId id, int width, int height, int fps, bool audio,
                       std::function<void(const std::string&)> done) = 0;
    /// Capture at another size without stopping.
    virtual void resize(int width, int height) = 0;
    virtual void setShowsCursor(bool show) = 0;
    /// Stop; `done` runs when stopped (any thread).
    virtual void stop(std::function<void()> done) = 0;
    /// Deliver the last picture again (the OS only delivers frames when the screen changes).
    virtual void resendLast() = 0;
};

enum class Codec : uint32_t { H264 = 0, Hevc = 1 };

class Encoder {
public:
    /// Hardware encoder tuned for latency; null if this codec/size isn't available.
    static std::unique_ptr<Encoder> create(int width, int height, int fps, int bitrate, Codec codec);
    virtual ~Encoder() = default;

    /// (Annex-B access unit, keyframe, Annex-B parameter sets on keyframes, capture time)
    std::function<void(Bytes, bool, std::optional<Bytes>, Clock::time_point)> onFrame;

    virtual Codec codec() const = 0;
    virtual int width() const = 0;
    virtual int height() const = 0;
    /// Frames of another size (while a resize settles) are scaled to the encoder's size.
    virtual void encode(const Frame& frame) = 0;
    virtual void requestKeyframe() = 0;
    virtual void setBitrate(int bitsPerSecond) = 0;
};

// MARK: Input

/// Touch, pen and gestures from the tablet as mouse, scroll and keyboard events.
class Pointer {
public:
    static std::unique_ptr<Pointer> create();
    virtual ~Pointer() = default;
    DisplayId display = 0;     // positions are fractions (0-1) of this display
    bool restoreCursor = true; // put the cursor back where it was after a touch

    virtual void touch(uint8_t action, float x, float y) = 0;
    virtual void scroll(uint8_t kind, uint8_t phase, bool last, float x, float y, float dx, float dy) = 0;
    virtual void zoom(int8_t direction, float x, float y) = 0;
    virtual void pen(uint8_t action, uint8_t buttons, float x, float y, float pressure) = 0;
    /// A touch gesture is using the cursor (so the stream leaves it out of the picture).
    virtual bool touchActive() const = 0;
};

// MARK: Sound

/// "Tablet Only" sound: mute the computer's own output (the captured sound still reaches the
/// tablet), remembering that we did; restore undoes it (also after a crash, on next launch).
void muteSpeakers();
void restoreSpeakers();

/// Plays 48 kHz 16-bit PCM from the tablet (its own sound, or its microphone into the virtual
/// microphone device). Keeps at most ~150 ms queued so it never drifts behind.
class PcmPlayer {
public:
    /// `deviceUid`: play into that device instead of the default output. Null if unavailable.
    static std::unique_ptr<PcmPlayer> create(int channels, const std::string& deviceUid = {});
    virtual ~PcmPlayer() = default;
    virtual void play(const Bytes& pcm) = 0;
};

/// The "Spanly Microphone" virtual device (an audio driver installed once, with admin rights).
constexpr const char* kMicrophoneUid = "com.caigenix.spanly.microphone";
bool microphoneInstalled();
void installMicrophone(); // asks for the administrator password

// MARK: The tablet's own screen

/// A window showing the tablet's screen (H.264 from the tablet), turning mouse and keyboard
/// input into taps, swipes and typing there. Main thread, except config/frame (any thread).
class VideoWindow {
public:
    struct Input {
        std::function<void(uint8_t action, float x, float y)> pointer; // 0 down, 1 move, 2 up, 3 long press
        std::function<void(float x, float y, float dx, float dy)> scroll;
        std::function<void(const Bytes& key)> key; // REMOTE_KEY payload: kind + data
        std::function<void()> closed;
    };
    static std::unique_ptr<VideoWindow> create(const std::string& title, Input input);
    virtual ~VideoWindow() = default;
    virtual void show() = 0;
    virtual void close() = 0;
    virtual void setVideoSize(int width, int height) = 0;
    /// Whether the tablet can be controlled from here (its accessibility service is on).
    virtual void setControlAvailable(bool available) = 0;
    virtual void config(const Bytes& annexB) = 0; // SPS + PPS
    virtual void frame(const Bytes& annexB) = 0;  // one access unit
};

/// spanly:// links (e.g. from Shortcuts): called with the whole URL.
void onOpenUrl(std::function<void(const std::string&)> handler);

// MARK: Menu bar / tray

/// One entry in the menu, rebuilt each time it opens (the platform draws it).
struct MenuItem {
    enum class Kind { Action, Label, Separator, Submenu };
    Kind kind = Kind::Action;
    std::string title;
    bool checked = false;
    bool enabled = true;
    bool bold = false;
    std::string key; // keyboard shortcut, e.g. "q"
    std::function<void()> action;
    std::vector<MenuItem> children;

    static MenuItem item(std::string title, std::function<void()> action, bool checked = false, bool enabled = true) {
        return {Kind::Action, std::move(title), checked, enabled, false, {}, std::move(action), {}};
    }
    static MenuItem label(std::string title, bool bold = false) {
        return {Kind::Label, std::move(title), false, false, bold, {}, {}, {}};
    }
    static MenuItem separator() { return {Kind::Separator, {}, false, true, false, {}, {}, {}}; }
    static MenuItem submenu(std::string title, std::vector<MenuItem> children) {
        return {Kind::Submenu, std::move(title), false, true, false, {}, {}, std::move(children)};
    }
};

enum class TrayState { Idle, Streaming, Warning };

class Tray {
public:
    /// `build` makes the menu each time it opens (main thread).
    static std::unique_ptr<Tray> create(std::function<std::vector<MenuItem>()> build);
    virtual ~Tray() = default;
    virtual void setState(TrayState state) = 0;
};

/// A modal question; true for the first button.
bool ask(const std::string& title, const std::string& message, const std::string& ok, const std::string& cancel);
void openFile(const std::string& path);

// MARK: System events

struct SystemEvents {
    std::function<void(bool awake)> displayPower; // the screens slept or woke
    std::function<void()> screensChanged;         // displays added, removed or rearranged
    std::function<void()> willQuit;
};
void watchSystem(SystemEvents events);
bool onBattery();

bool loginItemEnabled();
void setLoginItem(bool enabled);

/// Run a program and return what it printed (stdout and stderr); empty if it can't start.
std::string runProcess(const std::string& path, const std::vector<std::string>& args);
bool isExecutable(const std::string& path);
std::string homeDirectory();

// MARK: System

bool screenRecordingAllowed();
void requestScreenRecording();
bool accessibilityAllowed();
void requestAccessibility();

/// The computer's name as the user sees it (also the Wi-Fi service name).
std::string computerName();
std::string logFilePath();
/// Where settings live on Windows and Linux (macOS keeps them in the app's preferences).
std::string configDirectory();

/// Persistent settings (key -> value), shared with earlier versions of the app where possible.
class Store {
public:
    static Store& shared();
    virtual ~Store() = default;
    virtual std::optional<std::string> string(const std::string& key) = 0;
    virtual std::optional<Bytes> data(const std::string& key) = 0;
    virtual std::optional<bool> boolean(const std::string& key) = 0;
    virtual void set(const std::string& key, const std::string& value) = 0;
    virtual void set(const std::string& key, const Bytes& value) = 0;
    virtual void set(const std::string& key, bool value) = 0;
    virtual void remove(const std::string& key) = 0;
};

} // namespace spanly::platform
