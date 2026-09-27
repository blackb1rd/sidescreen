import Foundation

// MARK: - Options

struct Options {
    var displayName: String? // nil = create our own virtual display sized to the tablet
    var hiDPI: Bool? // nil = auto: HiDPI over the USB accessory link, standard over adb (~10-15 Mbit/s)
    var codec = "hevc"
    var batteryFps = 30 // frame rate while the Mac runs on battery (0 = same as --fps)
    var stats = false
    var position: String? // nil = the menu bar setting
    var lingerSeconds = 10.0
    var fps = 60
    var bitrateMbps: Double? // nil = auto: 20 over USB accessory, 6 over adb (stays under its limit)
    var port: UInt16 = 27183
    var maxWidth = 0 // 0 = display's native pixel width
    var manageAdb = true
    var restoreCursor: Bool? // nil = the menu bar setting
    var usb = true

    static func parse() -> Options {
        var o = Options()
        var args = CommandLine.arguments.dropFirst().makeIterator()
        while let a = args.next() {
            switch a {
            case "--display": o.displayName = args.next()
            case "--hidpi": o.hiDPI = true
            case "--standard": o.hiDPI = false
            case "--codec": o.codec = args.next() ?? o.codec
            case "--battery-fps": o.batteryFps = Int(args.next() ?? "") ?? o.batteryFps
            case "--stats": o.stats = true
            case "--position": o.position = args.next()
            case "--fps": o.fps = Int(args.next() ?? "") ?? o.fps
            case "--bitrate": o.bitrateMbps = Double(args.next() ?? "")
            case "--port": o.port = UInt16(args.next() ?? "") ?? o.port
            case "--max-width": o.maxWidth = Int(args.next() ?? "") ?? o.maxWidth
            case "--no-adb": o.manageAdb = false
            case "--no-restore-cursor": o.restoreCursor = false
            case "--no-usb": o.usb = false
            case "-h", "--help":
                print("""
                usage: sidescreen [--hidpi | --standard] [--codec hevc|h264] [--position right|left|above|below|keep] [--display NAME]
                                  [--fps N] [--battery-fps N] [--bitrate MBPS] [--max-width PX] [--port N] [--stats]
                                  [--no-usb] [--no-adb] [--no-restore-cursor]
                Options override the menu bar settings for this run only.
                  --hidpi      always use a Retina virtual display (Automatic: Retina over USB accessory)
                  --standard   always use a non-Retina display at half the tablet's resolution (Automatic: over adb)
                  --codec      hevc (default; falls back to h264 if the tablet can't decode it) or h264
                  --position   right|left|above|below next to the main display, or keep
                  --display    stream an existing display with this name instead of creating a virtual one
                  --fps        capture frame rate (default 60)
                  --battery-fps  frame rate while the Mac is on battery (default 30; 0 = same as --fps)
                  --bitrate    video bitrate in Mbit/s (default 20 over USB accessory, 6 over adb)
                  --stats      log frame rate, encode time and bitrate every 5 seconds
                  --max-width  downscale if the display is wider than this many pixels
                  --no-usb     don't use USB accessory mode; stream through adb only (slower)
                  --no-adb     don't manage adb reverse / app launch / tablet sleep+wake
                  --no-restore-cursor  leave the cursor on the tablet screen after a touch
                """)
                exit(0)
            default:
                log("unknown option \(a)")
                exit(2)
            }
        }
        return o
    }
}
