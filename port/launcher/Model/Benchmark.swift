// SPDX-License-Identifier: GPL-2.0-or-later
// ISS benchmark: the game starts from a fixed save at the truck crash (port/benchmark/savedata, the
// heaviest place of the tuning runs: fire, smoke, sparks), nobody touches the controls, and after
// `duration` seconds the launcher stops it. The game logs "Frame stats: <fps> FPS, worst frame <ms> ms"
// every 5 s; the last `measured` windows (after loading and shader warm-up) make the result.
import Foundation

enum Benchmark {
    static let duration: TimeInterval = 90
    static let measured = 8 // windows of 5 s: the last 40 s

    /// The bundled save (checkout or app Resources), nil when this build has none.
    static func save(port: URL) -> URL? {
        let url = port.appendingPathComponent("benchmark/savedata")
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    struct Window: Equatable {
        var fps: Double
        var worstFrameMs: Double
    }

    struct Result: Equatable {
        var windows: [Window]
        var medianFPS: Double { median(windows.map(\.fps)) }
        var minFPS: Double { windows.map(\.fps).min() ?? 0 }
        var maxFPS: Double { windows.map(\.fps).max() ?? 0 }
        var medianWorstFrameMs: Double { median(windows.map(\.worstFrameMs)) }

        private func median(_ values: [Double]) -> Double {
            let sorted = values.sorted()
            guard !sorted.isEmpty else { return 0 }
            return sorted.count % 2 == 1 ? sorted[sorted.count / 2]
                : (sorted[sorted.count / 2 - 1] + sorted[sorted.count / 2]) / 2
        }
    }

    /// Every "Frame stats:" window of a session log, oldest first.
    static func windows(log: String) -> [Window] {
        var out: [Window] = []
        for line in log.split(separator: "\n") {
            guard let at = line.range(of: "Frame stats: ") else { continue }
            let rest = line[at.upperBound...]
            let fields = rest.split(separator: " ")
            guard fields.count >= 5, let fps = Double(fields[0]), fields[1].hasPrefix("FPS") else { continue }
            var worst = 0.0
            if let w = rest.range(of: "worst frame ") {
                worst = Double(rest[w.upperBound...].prefix { $0.isNumber || $0 == "." }) ?? 0
            }
            out.append(Window(fps: fps, worstFrameMs: worst))
        }
        return out
    }

    /// The result of a run: nil when the game logged too few windows (it did not reach the scene).
    static func result(log: String) -> Result? {
        let all = windows(log: log)
        guard all.count >= measured + 2 else { return nil }
        return Result(windows: Array(all.suffix(measured)))
    }

    /// A new GitHub issue with the result filled in (the player reviews it and presses Submit):
    /// no token in the app, nothing is sent without them.
    static func issueURL(_ r: Result, system: SystemInfo, report: String) -> URL? {
        var c = URLComponents(url: CrashReport.newIssue, resolvingAgainstBaseURL: false)
        let title = "Benchmark: \(system.chip), \(String(format: "%.1f", r.medianFPS)) FPS"
        let body = "```\n\(report)```\n"
        c?.queryItems = [URLQueryItem(name: "title", value: title), URLQueryItem(name: "body", value: body),
                         URLQueryItem(name: "labels", value: "benchmark")]
        return c?.url
    }

    static func report(_ r: Result, system: SystemInfo, settings: [String], date: Date = Date()) -> String {
        let f = { (v: Double) in String(format: "%.1f", v) }
        let stamp = ISO8601DateFormatter().string(from: date)
        return """
        ISS benchmark (truck crash, standing still), \(stamp)
        FPS: median \(f(r.medianFPS)), min \(f(r.minFPS)), max \(f(r.maxFPS)) (last \(r.windows.count) windows of 5 s)
        Worst frame per 5 s: median \(f(r.medianWorstFrameMs)) ms
        Mac: \(system.line)
        Settings: \(settings.joined(separator: ", "))
        Windows: \(r.windows.map { f($0.fps) }.joined(separator: " "))

        """
    }
}
