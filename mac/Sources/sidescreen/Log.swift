import Foundation

let logURL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/SideScreen.log")

/// Logs to the terminal when run from one, otherwise to ~/Library/Logs/SideScreen.log.
private let logFile: FileHandle? = {
    guard isatty(STDOUT_FILENO) == 0 else { return nil }
    let fm = FileManager.default
    try? fm.createDirectory(at: logURL.deletingLastPathComponent(), withIntermediateDirectories: true)
    // Keep the log from growing forever.
    if let size = try? fm.attributesOfItem(atPath: logURL.path)[.size] as? Int, size > 5_000_000 {
        try? fm.removeItem(at: logURL)
    }
    if !fm.fileExists(atPath: logURL.path) { fm.createFile(atPath: logURL.path, contents: nil) }
    let h = try? FileHandle(forWritingTo: logURL)
    _ = try? h?.seekToEnd()
    return h
}()

func log(_ s: String) {
    let t = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
    let line = "[\(t)] \(s)"
    if let logFile {
        logFile.write(Data((line + "\n").utf8))
    } else {
        print(line)
    }
}
