// SPDX-License-Identifier: GPL-2.0-or-later
// Detailed benchmark: the benchmark scene twice, at the player's settings and at half render scale,
// read from the game's "Perf:" lines (one per 5 s: key=value tokens, unknown keys ignored, any key may
// be missing; see docs/PERF_STRATEGY.md "Player report"). The two runs give a verdict: GPU-bound,
// CPU-bound, frame rate cap or mixed. Pure functions only; LauncherModel runs the games.
import Foundation

/// One 5 s window of the game's "Perf:" line.
struct PerfWindow: Equatable {
    var values: [String: Double]
    subscript(key: String) -> Double? { values[key] }
}

enum DetailedBenchmark {
    static let phase1Duration: TimeInterval = Benchmark.duration
    static let phase1Measured = Benchmark.measured
    /// Phase 2 loads the scene again: 80 s, the last 30 s (6 windows) measured.
    static let phase2Duration: TimeInterval = 80
    static let phase2Measured = 6

    // MARK: Parsing

    /// "Perf: fps=59.8 cpu.main=48 ..." anywhere in a log line; nil when the line has none.
    static func parse(line: Substring) -> PerfWindow? {
        guard let at = line.range(of: "Perf: ") else { return nil }
        var values: [String: Double] = [:]
        for token in line[at.upperBound...].split(whereSeparator: { $0 == " " || $0 == "\t" }) {
            guard let eq = token.firstIndex(of: "="), eq != token.startIndex,
                  let v = Double(token[token.index(after: eq)...]), v.isFinite else { continue }
            values[String(token[..<eq])] = v
        }
        return values.isEmpty ? nil : PerfWindow(values: values)
    }

    /// Every "Perf:" window of a session log, oldest first.
    static func windows(log: String) -> [PerfWindow] {
        log.split(separator: "\n").compactMap(parse(line:))
    }

    /// One run's measured windows: the last `measured` Perf windows, or (an older game build without
    /// Perf lines) the Frame stats windows with fps and the worst frame only. nil: too few windows.
    static func phase(log: String, measured: Int, renderScale: Int, duration: TimeInterval) -> Phase? {
        var all = windows(log: log)
        var hasPerf = true
        if all.isEmpty {
            hasPerf = false
            all = Benchmark.windows(log: log).map { PerfWindow(values: ["fps": $0.fps, "ft_max": $0.worstFrameMs]) }
        }
        guard all.count >= measured + 2 else { return nil }
        var phase = Phase(renderScale: renderScale, duration: duration, hasPerf: hasPerf,
                          windows: Array(all.suffix(measured)), warmup: Array(all.dropLast(measured)))
        phase.surface = renderSurface(log: log)
        return phase
    }

    /// The size the game renders at: the "Native rendering: ... scene/output surface WxH" line, or the
    /// original 1920x1080 when the native patch is off.
    static func renderSurface(log: String) -> (width: Int, height: Int) {
        for line in log.split(separator: "\n") where line.contains("Native rendering: output selection") {
            if let r = line.range(of: #"surface (\d+)x(\d+)"#, options: .regularExpression) {
                let dims = line[r].dropFirst("surface ".count).split(separator: "x").compactMap { Int($0) }
                if dims.count == 2 { return (dims[0], dims[1]) }
            }
        }
        return (1920, 1080)
    }

    // MARK: Aggregation

    struct Phase: Equatable {
        static func == (a: Phase, b: Phase) -> Bool {
            a.renderScale == b.renderScale && a.duration == b.duration && a.hasPerf == b.hasPerf
                && a.windows == b.windows && a.warmup == b.warmup && a.surface.width == b.surface.width
                && a.surface.height == b.surface.height
        }
        var surface: (width: Int, height: Int) = (1920, 1080) // what the game rendered at (from the log)
        var pixels: Int { surface.width * surface.height }
        var renderScale: Int // percent of 1080p
        var duration: TimeInterval
        var hasPerf: Bool
        var windows: [PerfWindow]
        /// The windows before the measured ones (loading, shader warm-up): only for thermal/memory peaks.
        var warmup: [PerfWindow] = []

        /// Median over the windows that have the key (ft_p99, ft_p999, ft_max: this and `worst`).
        func median(_ key: String) -> Double? { Phase.median(windows.compactMap { $0[key] }) }
        /// The worst (largest) value over the windows.
        func worst(_ key: String) -> Double? { windows.compactMap { $0[key] }.max() }
        var keys: [String] { Array(Set(windows.flatMap { $0.values.keys })).sorted() }
        var fps: Double? { median("fps") }

        static func median(_ v: [Double]) -> Double? {
            let s = v.sorted()
            guard !s.isEmpty else { return nil }
            return s.count % 2 == 1 ? s[s.count / 2] : (s[s.count / 2 - 1] + s[s.count / 2]) / 2
        }

        /// cpu.<thread> medians, busiest first.
        var threads: [(name: String, percent: Double)] {
            keys.filter { $0.hasPrefix("cpu.") }.compactMap { k in median(k).map { (String(k.dropFirst(4)), $0) } }
                .sorted { $0.percent > $1.percent }
        }
    }

    // MARK: Verdict

    enum Kind: String { case frameRateCap, gpuBound, cpuBound, mixed, needsNewerBuild, notEnoughData }

    struct Verdict: Equatable {
        var kind: Kind
        var text: String
        var advice: String
    }

    static let capTolerance = 0.03
    static let gpuGain = 0.40
    static let cpuGain = 0.10
    static let gpuBusyBound = 90.0

    /// `cap`: the frame rate cap in force (the display's rate when the setting is Display), 0 = unknown.
    static func verdict(full: Phase, half: Phase?, cap: Int) -> Verdict {
        guard let fps = full.fps, fps > 0 else {
            return Verdict(kind: .notEnoughData, text: "Not enough data", advice: "")
        }
        // The half-scale run counts only when it really rendered fewer pixels (an older game build
        // ignored the render scale with a native output resolution set).
        let halfApplied = half.map { $0.pixels * 4 <= full.pixels * 3 } ?? false
        let gain = halfApplied ? half?.fps.map { $0 / fps - 1 } : nil
        let gainText = gain.map { String(format: "half render scale %+.0f%% FPS", $0 * 100) }
        guard full.hasPerf else {
            var t = "Verdict needs a newer game build (no Perf lines in the log): \(f1(fps)) FPS"
            if let gainText { t += ", \(gainText)" }
            return Verdict(kind: .needsNewerBuild, text: t, advice: "")
        }
        let gpu = full.median("gpu_busy")
        let gpuText = gpu.map { String(format: "GPU busy %.0f%%", $0) }
        if cap > 0 && fps >= Double(cap) * (1 - capTolerance) {
            return Verdict(kind: .frameRateCap, text: "Frame rate cap (\(cap) FPS held" + (gpuText.map { ", \($0)" } ?? "") + ")",
                           advice: "Your Mac had room to spare: you can raise detail or the frame rate cap.")
        }
        // The port's own named threads (DrawRec, VkRecorder...); unnamed ones are mostly the game's.
        let thread = full.threads.first(where: { !$0.name.hasSuffix("unnamed") })
            .map { String(format: "busiest port thread %@ %.0f%%", $0.name, $0.percent) }
        func parts(_ items: String?...) -> String { items.compactMap { $0 }.joined(separator: ", ") }
        if (gain ?? 0) >= gpuGain || (gpu ?? 0) >= gpuBusyBound {
            return Verdict(kind: .gpuBound, text: "GPU-bound (" + parts(gpuText, gainText) + ")",
                           advice: "The graphics chip is the limit: a lower render scale, an upscaler or lower detail gives more frames.")
        }
        if let gain, gain <= cpuGain {
            return Verdict(kind: .cpuBound, text: "CPU-bound (" + parts(gainText) + (thread.map { "; \($0)" } ?? "") + ")",
                           advice: "The processor side (the game's own work and the port's draw recording) is the limit: lowering the render scale won't help much; lower detail or a frame rate cap just under your FPS keeps motion steadier.")
        }
        if gain == nil, let gpu {
            // One run only: the GPU's busy share decides.
            if gpu < 70 {
                return Verdict(kind: .cpuBound, text: "CPU-bound (" + parts(gpuText) + (thread.map { "; \($0)" } ?? "") + ")",
                               advice: "The graphics chip waited for work: the processor side is the limit.")
            }
        }
        guard gain != nil || gpu != nil else {
            return Verdict(kind: .notEnoughData, text: "Not enough data", advice: "")
        }
        return Verdict(kind: .mixed, text: "Mixed, GPU and CPU both limit (" + parts(gpuText, gainText) + (thread.map { "; \($0)" } ?? "") + ")",
                       advice: "Neither side alone is the limit: lower detail helps a little on both.")
    }

    // MARK: Memory and thermal

    static let pressureNames = ["normal", "warning", "critical"]
    static let thermalNames = ["nominal", "fair", "serious", "critical"]

    static func memoryLine(phases: [Phase], memoryGB: Int) -> String? {
        let all = phases.flatMap { $0.warmup + $0.windows }
        let totals = all.compactMap { w -> Double? in
            guard w["fp_probe_mb"] != nil || w["fp_host_mb"] != nil else { return nil }
            return (w["fp_probe_mb"] ?? 0) + (w["fp_host_mb"] ?? 0)
        }
        let pressure = all.compactMap { $0["pressure"] }.max()
        let swap = all.compactMap { $0["swap_mb"] }.max()
        var items: [String] = []
        if let peak = totals.max() { items.append(String(format: "%.1f of %d GB", peak / 1024, memoryGB)) }
        if let pressure { items.append("pressure " + name(pressureNames, pressure)) }
        if let swap { items.append(swap > 0 ? String(format: "swap %.0f MB used", swap) : "no swap") }
        return items.isEmpty ? nil : "Memory: " + items.joined(separator: ", ")
    }

    static func thermalLine(phases: [Phase]) -> String? {
        let all = phases.flatMap { $0.warmup + $0.windows }.compactMap { $0["therm"] }
        guard let first = all.first, let worst = all.max() else { return nil }
        let a = name(thermalNames, first), b = name(thermalNames, worst)
        return worst > first ? "Thermal: \(a) -> \(b)" : "Thermal: \(a)"
    }

    private static func name(_ names: [String], _ v: Double) -> String { names[min(max(Int(v), 0), names.count - 1)] }
    private static func f1(_ v: Double) -> String { String(format: "%.1f", v) }

    // MARK: Report

    /// The Mac, without anything that identifies its owner (no user name, host name, serial, paths).
    struct Machine: Equatable {
        var system: SystemInfo
        var display: String?      // e.g. "3456x2234 at 120 Hz"
        var power: PowerState?
        var lowPowerMode: Bool

        var powerLine: String {
            var items: [String] = []
            if let power { items.append(power.onAC ? "power adapter" : "battery") }
            items.append("Low Power Mode " + (lowPowerMode ? "on" : "off"))
            if let power {
                switch power.mode {
                case .automatic: items.append("Energy Mode automatic")
                case .low: items.append("Energy Mode low")
                case .high: items.append("Energy Mode high")
                case .lowPowerOnly: break
                }
            }
            return items.joined(separator: ", ")
        }
    }

    struct Report {
        var full: Phase
        var half: Phase?
        var cap: Int
        var machine: Machine
        var settings: [String]
        var date: Date

        var verdict: Verdict { DetailedBenchmark.verdict(full: full, half: half, cap: cap) }
        var phases: [Phase] { [full] + (half.map { [$0] } ?? []) }

        var title: String {
            "Detailed benchmark: \(machine.system.chip), \(full.fps.map(f1) ?? "?") FPS, \(verdict.text.prefix { $0 != "(" && $0 != ":" }.trimmingCharacters(in: .whitespaces))"
        }

        func phaseLine(_ label: String, _ p: Phase) -> String {
            var s = "\(label) (\(p.surface.width)x\(p.surface.height)): \(p.fps.map(f1) ?? "?") FPS median"
            if let lo = p.windows.compactMap({ $0["fps"] }).min(), let hi = p.windows.compactMap({ $0["fps"] }).max() {
                s += " (\(f1(lo))-\(f1(hi)))"
            }
            if let p50 = p.median("ft_p50"), let p99 = p.median("ft_p99") {
                s += ", frame time p50 \(f1(p50)) / p99 \(f1(p99))"
                if let p999 = p.median("ft_p999") { s += " / p99.9 \(f1(p999))" }
                s += " ms"
            }
            if let g = p.median("gpu_busy") { s += String(format: ", GPU busy %.0f%%", g) }
            if let h = p.median("held") { s += String(format: ", held frames %.0f per window", h) }
            return s
        }

        var text: String {
            var lines = ["ISS detailed benchmark (truck crash, standing still), \(ISO8601DateFormatter().string(from: date))",
                         "Verdict: \(verdict.text)."]
            if !verdict.advice.isEmpty { lines.append(verdict.advice) }
            if let m = DetailedBenchmark.memoryLine(phases: phases, memoryGB: machine.system.memoryGB) { lines.append(m) }
            if let t = DetailedBenchmark.thermalLine(phases: phases) { lines.append(t) }
            lines.append("")
            lines.append(phaseLine("Your settings", full))
            if let half { lines.append(phaseLine("Half render scale", half)) }
            else { lines.append("Half render scale: no result (the second run did not reach the scene).") }
            let threads = full.threads.prefix(4).map { String(format: "%@ %.0f%%", $0.name, $0.percent) }
            if !threads.isEmpty { lines.append("Port threads (% of one core): " + threads.joined(separator: ", ")) }
            lines.append("Mac: \(machine.system.line)")
            if let d = machine.display { lines.append("Display: \(d)") }
            lines.append("Power: \(machine.powerLine)")
            lines.append("Settings: \(settings.joined(separator: ", "))")
            return lines.joined(separator: "\n") + "\n"
        }

        /// All windows of both phases, the settings and the Mac, as JSON text.
        var json: String {
            func phaseObject(_ p: Phase) -> [String: Any] {
                var median: [String: Double] = [:], worst: [String: Double] = [:]
                for k in p.keys {
                    if let v = p.median(k) { median[k] = v }
                    if let v = p.worst(k) { worst[k] = v }
                }
                return ["renderScalePercent": p.renderScale, "renderWidth": p.surface.width, "renderHeight": p.surface.height, "durationSeconds": p.duration, "perfLines": p.hasPerf,
                        "windows": p.windows.map(\.values), "median": median, "worst": worst]
            }
            var phases: [String: Any] = ["yourSettings": phaseObject(full)]
            if let half { phases["halfRenderScale"] = phaseObject(half) }
            let v = verdict
            var system: [String: Any] = ["model": machine.system.model, "chip": machine.system.chip,
                                         "memoryGB": machine.system.memoryGB, "macOS": machine.system.macOS,
                                         "lowPowerMode": machine.lowPowerMode, "power": machine.powerLine]
            if let d = machine.display { system["display"] = d }
            let root: [String: Any] = [
                "version": 1, "date": ISO8601DateFormatter().string(from: date), "app": machine.system.appVersion,
                "frameRateCap": cap, "settings": settings, "mac": system, "phases": phases,
                "verdict": ["kind": v.kind.rawValue, "text": v.text, "advice": v.advice],
            ]
            let data = (try? JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted, .sortedKeys])) ?? Data()
            return String(decoding: data, as: UTF8.self) + "\n"
        }

        /// A new GitHub issue with the summary filled in (the player reviews it and presses Submit).
        var issueURL: URL? {
            var c = URLComponents(url: CrashReport.newIssue, resolvingAgainstBaseURL: false)
            c?.queryItems = [URLQueryItem(name: "title", value: title), URLQueryItem(name: "body", value: "```\n\(text)```\n"),
                             URLQueryItem(name: "labels", value: "benchmark")]
            return c?.url
        }
    }

    /// Phase 2's scale: half of the player's (25% at the least).
    static func halfScale(of scale: RenderScale) -> Int {
        max(25, (Int(scale.rawValue) ?? 100) / 2)
    }
}
