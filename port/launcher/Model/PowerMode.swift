// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation

/// macOS's Energy Mode as `pmset -g` reports it for the current power source, and the launcher's
/// hint about it. On a 16" M3 Max (2026-11-14) the default "Automatic" mode let the CPU clock down
/// after ~3.5 minutes of play: bridge 101 -> 83 FPS, truck crash 75 -> 61. With "High Power" both
/// held for 9 minutes (102, 75). The launcher only suggests the setting; it never changes it.
enum EnergyMode: Equatable {
    case automatic, low, high
    /// Older systems and Macs without Energy Mode: only Low Power Mode on or off.
    case lowPowerOnly(on: Bool)
}

struct PowerState: Equatable {
    var mode: EnergyMode
    var onAC: Bool

    /// Parses `pmset -g` (current settings) and `pmset -g ps` (power source) output.
    static func parse(settings: String, source: String) -> PowerState? {
        var mode: EnergyMode?
        for line in settings.split(separator: "\n") {
            let parts = line.split(whereSeparator: { $0 == " " || $0 == "\t" })
            guard parts.count >= 2, let value = Int(parts[1]) else { continue }
            switch parts[0] {
            case "powermode":
                mode = value == 2 ? .high : value == 1 ? .low : .automatic
            case "lowpowermode" where mode == nil:
                mode = .lowPowerOnly(on: value == 1)
            default:
                break
            }
        }
        guard let mode else { return nil }
        return PowerState(mode: mode, onAC: source.contains("'AC Power'"))
    }

    /// The hint for the launcher, or nil when nothing needs saying.
    var hint: String? {
        switch mode {
        case .high:
            return nil
        case .low, .lowPowerOnly(on: true):
            return "Low Power Mode is on: the game runs much slower. Turn it off in System Settings → Battery."
        case .lowPowerOnly(on: false):
            return nil
        case .automatic:
            guard onAC else { return nil }
            return "For long sessions on a MacBook Pro, set System Settings → Battery → Energy Mode to High Power (if your Mac offers it). In Automatic, macOS lowers the clocks after a few minutes of play: 15-20% fewer FPS."
        }
    }

    /// Reads the current state with /usr/bin/pmset (no privileges needed); nil if it cannot.
    static func current() -> PowerState? {
        func run(_ args: [String]) -> String? {
            let process = Process()
            process.executableURL = URL(fileURLWithPath: "/usr/bin/pmset")
            process.arguments = args
            let pipe = Pipe()
            process.standardOutput = pipe
            process.standardError = FileHandle.nullDevice
            do { try process.run() } catch { return nil }
            let data = pipe.fileHandleForReading.readDataToEndOfFile()
            process.waitUntilExit()
            return String(data: data, encoding: .utf8)
        }
        guard let settings = run(["-g"]), let source = run(["-g", "ps"]) else { return nil }
        return parse(settings: settings, source: source)
    }
}
