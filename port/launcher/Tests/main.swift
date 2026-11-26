// SPDX-FileCopyrightText: Copyright 2026 satperm
// SPDX-License-Identifier: GPL-2.0-or-later
// Model tests for the launcher (scripts/macos/build_launcher.sh --test, run from port/).
// One PASS line, or FAIL lines and exit 1.
import Foundation

var failures = 0
func check(_ condition: @autoclosure () -> Bool, _ what: String, line: Int = #line) {
    if !condition() {
        failures += 1
        print("FAIL (launcher/Tests/main.swift:\(line)): \(what)")
    }
}
func button(_ key: String) -> KeyButton { KeyMap.all.first { $0.key == key }! }

// KeyMap: defaults write nothing; only changed buttons are written; round trip.
do {
    let map = KeyMap()
    check(map.isDefault && map.string == nil, "defaults produce no variable")
    check(KeyMap(string: nil).isDefault && KeyMap(string: "").isDefault, "empty value is default")
    var custom = KeyMap()
    custom.set(button("cross"), slot: 0, to: "Left Ctrl")
    custom.set(button("triangle"), slot: 0, to: "Q") // same as the default: still default
    custom[button("r2")] = ["Mouse X1", "Wheel Up"]
    custom[button("l3")] = []
    check(custom.string == "cross=Left Ctrl,r2=Mouse X1|Wheel Up,l3=none", "serialised: \(custom.string ?? "nil")")
    check(KeyMap(string: custom.string) == custom, "round trip")
    check(KeyMap(string: "cross=Left Ctrl")[button("cross")] == ["Left Ctrl"], "parse one")
    check(KeyMap(string: "cross=Space|F")[button("cross")] == ["Space", "F"], "parse several")
    check(KeyMap(string: "l2=R|Mouse Right").isDefault, "default spelled out is default")
    check(KeyMap(string: "r2=Mouse Left|F").isDefault, "order does not matter")
    var reset = custom
    reset.reset(button("cross"))
    check(reset[button("cross")] == ["Space"], "reset one button")
}

// The game's own spelling: case, spaces, the touchpad alias, none.
do {
    let map = KeyMap(string: " Touchpad = tab|mouse middle , CIRCLE=none ")
    check(map[button("touchpad_left")] == ["Tab", "Mouse Middle"], "alias, case and spaces: \(map[button("touchpad_left")])")
    check(map[button("circle")] == [], "none unbinds")
    check(map.string == "circle=none,touchpad_left=Tab|Mouse Middle", "alias written as touchpad_left: \(map.string ?? "nil")")
}

// Bad names are rejected; a button whose names are all bad keeps its default.
do {
    check(KeyMap.canonical("Left Shift") == "Left Shift" && KeyMap.canonical("left shift") == "Left Shift", "canonical name")
    check(KeyMap.canonical("Banana") == nil, "unknown name")
    check(KeyMap.canonical("Escape") == nil && KeyMap.canonical("Left GUI") == nil, "reserved keys")
    check(KeyMap.canonical("Wheel Down") == "Wheel Down" && KeyMap.canonical("Mouse X2") == "Mouse X2", "mouse names")
    let map = KeyMap(string: "cross=Banana,square=Banana|Z,nothing=Q,circle")
    check(map[button("cross")] == ["Space"], "all bad: default stays")
    check(map[button("square")] == ["Z"], "bad name dropped, good one kept")
    check(map.isDefault == false && map.string == "square=Z", "unknown button and entry without = ignored: \(map.string ?? "nil")")
    for (_, name) in KeyCodes.table { check(KeyMap.canonical(name) != nil || KeyMap.reserved.contains(name), "key name \(name)") }
}

// Conflicts: the same key on two buttons.
do {
    var map = KeyMap()
    check(map.conflicts.isEmpty, "no conflict by default")
    map[button("cross")] = ["E"]
    let found = map.conflicts
    check(found.count == 1 && found[0].name == "E" && found[0].buttons.map(\.key) == ["cross", "square"], "conflict found: \(found)")
    check(map.others(using: "E", besides: button("cross")).map(\.key) == ["square"], "others")
    check(KeyMap.fixedUse(of: "Left Ctrl") != nil && KeyMap.fixedUse(of: "W") == nil, "fixed keys")
    // Actions share the namespace: W on a button clashes with move_up; swipe + touchpad is by design.
    map = KeyMap()
    map[button("cross")] = ["W"]
    check(map.conflicts.map(\.name) == ["W"], "action conflict")
    map = KeyMap()
    map[button("touchpad_left")] = ["F2"]
    check(map.conflicts.isEmpty && map[button("swipe")] == ["F2"], "swipe follows the touchpad click")
    map[button("swipe")] = ["F2"]
    check(map.conflicts.isEmpty, "swipe = touchpad click is no conflict")
}

// Actions: defaults, serialisation, swipe following the touchpad click, parsing.
do {
    check(KeyMap.actions.count == 12 && KeyMap().string == nil, "12 actions, default writes nothing")
    var map = KeyMap()
    map[button("move_up")] = ["Up"]
    map[button("can_shake")] = []
    check(map.string == "move_up=Up,can_shake=none", "actions serialised: \(map.string ?? "nil")")
    check(KeyMap(string: map.string) == map, "actions round trip")
    check(KeyMap(string: "LOOK_LEFT=J|Mouse X1")[button("look_left")] == ["J", "Mouse X1"], "action parse, any case")
    check(KeyMap(string: "move_forward=W,look_up=Banana").isDefault, "unknown action and name dropped")
    var swipe = KeyMap()
    check(swipe[button("swipe")] == ["Tab"] && swipe.isDefault(button("swipe")), "swipe defaults to Tab")
    swipe[button("touchpad_left")] = ["Delete"]
    check(swipe.string == "touchpad_left=Delete" && swipe[button("swipe")] == ["Delete"], "swipe not written while it follows")
    swipe.set(button("swipe"), slot: 0, to: "Left Alt")
    check(swipe.string == "touchpad_left=Delete,swipe=Left Alt", "swipe explicit: \(swipe.string ?? "nil")")
    swipe.reset(button("swipe"))
    check(swipe.string == "touchpad_left=Delete", "swipe reset follows again")
    check(KeyMap().sprayHelp() .contains("hold T or G") && KeyMap().touchpadHelp().contains("hold Tab and press an arrow key"), "default help wording")
    var help = KeyMap()
    help[button("can_left")] = ["V"]
    help[button("swipe")] = ["Left Alt"]
    check(help.sprayHelp().contains("hold V or G") && help.touchpadHelp().contains("hold Left Alt and press a camera key"), "help names current keys")
}

// The longest possible value stays under the game's 4096-byte buffer.
do {
    var map = KeyMap()
    for b in KeyMap.all { map[b] = ["Keypad Enter", "Keypad .", "NonUSBackslash", "Right Shift"] }
    check(map[button("cross")].count == KeyMap.slots, "slots capped")
    check(map.fits && (map.string ?? "").utf8.count < 4096, "longest string \((map.string ?? "").utf8.count) bytes")
}

// LaunchSettings: launch.conf round trip, summary.
do {
    var settings = LaunchSettings()
    check(!settings.text.contains("TLG_KEY_MAP") && !settings.summary.contains("custom keys"), "default: no variable, no summary")
    settings.keys[button("cross")] = ["Left Ctrl"]
    check(settings.text.contains("TLG_KEY_MAP='cross=Left Ctrl'"), "written quoted")
    check(settings.summary.contains("custom keys"), "summary")
    check(settings.environment["TLG_KEY_MAP"] == "cross=Left Ctrl", "environment for the game")
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("launcher-test-\(getpid())/launch.conf")
    try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try? settings.write(to: url)
    check(LaunchSettings(contentsOf: url).keys == settings.keys, "read back from launch.conf")
    settings.keys = KeyMap()
    check(!settings.text.contains("TLG_KEY_MAP"), "reset removes the line")
    try? FileManager.default.removeItem(at: url.deletingLastPathComponent())
}

// The Swift defaults equal the key_map table in src/runtime_pad.c.
do {
    let path = "src/runtime_pad.c"
    if let c = try? String(contentsOfFile: path, encoding: .utf8),
       let start = c.range(of: "static ButtonMap key_map={{"),
       let end = c[start.upperBound...].range(of: "\n},") {
        let table = c[start.upperBound..<end.lowerBound]
        let scancodes = ["UP": "Up", "DOWN": "Down", "LEFT": "Left", "RIGHT": "Right", "SPACE": "Space", "LSHIFT": "Left Shift", "RETURN": "Return", "TAB": "Tab", "BACKSPACE": "Backspace"]
        let mice = ["MOUSE_LEFT": "Mouse Left", "MOUSE_RIGHT": "Mouse Right", "MOUSE_MIDDLE": "Mouse Middle"]
        let buttons = ["CROSS": "cross", "CIRCLE": "circle", "SQUARE": "square", "TRIANGLE": "triangle", "L1": "l1", "R1": "r1",
                       "L2": "l2", "R2": "r2", "L3": "l3", "R3": "r3", "OPTIONS": "options", "UP": "up", "DOWN": "down",
                       "LEFT": "left", "RIGHT": "right"]
        var fromC: [String: [String]] = [:]
        var entries = 0
        for chunk in table.split(separator: "{").dropFirst() {
            let pair = chunk.split(whereSeparator: { ",}".contains($0) }).map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            guard pair.count >= 2 else { continue }
            let src = pair[0], ps = pair[1]
            let name: String?
            if src.hasPrefix("SDL_SCANCODE_") {
                let s = String(src.dropFirst("SDL_SCANCODE_".count))
                name = s.count == 1 ? s : scancodes[s]
            } else {
                name = mice[src]
            }
            let key: String? = ps.hasPrefix("ACTION(ACT_") ? String(ps.dropFirst("ACTION(ACT_".count).dropLast()).lowercased()
                : ps == "CLICK_LEFT" ? "touchpad_left" : ps == "CLICK_RIGHT" ? "touchpad_right"
                : buttons[String(ps.dropFirst("BTN_".count))]
            if let name, let key { fromC[key, default: []].append(name); entries += 1 } else { check(false, "unparsed key_map entry \(src),\(ps)") }
        }
        check(entries == 31, "key_map has \(entries) entries (31 expected)")
        check(Set(fromC.keys) == Set(KeyMap.all.filter { $0 != KeyMap.swipe }.map(\.key)), "same buttons: \(fromC.keys.sorted())")
        for b in KeyMap.all where b != KeyMap.swipe { check(fromC[b.key] == b.defaults, "\(b.key): C \(fromC[b.key] ?? []) vs Swift \(b.defaults)") }
        // ps_names: every Swift button is a name the game parses.
        for b in KeyMap.buttons { check(c.contains("{\"\(b.key)\","), "ps_names has \(b.key)") }
        for b in KeyMap.actions { check(c.contains("{\"\(b.key)\",ACTION("), "action_names has \(b.key)") }
        check(KeyMap.swipe.defaults == (fromC["touchpad_left"] ?? []), "swipe default = left touchpad click's keys")
        for (alias, target) in KeyMap.aliases { check(c.contains("{\"\(alias)\",CLICK_LEFT}") && target == "touchpad_left", "alias \(alias)") }
    } else {
        check(false, "cannot read src/runtime_pad.c (run from port/)")
    }
}

// Detailed benchmark: Perf line parsing, aggregation, verdict, report privacy.
do {
    let sample = "2026-11-26T10:00:00Z <6> Perf: fps=59.8 ft_p50=16.6 ft_p99=21.0 ft_p999=34.2 ft_max=41.0 held=2 gpu_busy=71 cpu.main=48 cpu.DrawScan=31 cpu.DrawRec=62 cpu.VkRecorder=55 cpu.ShaderCompile=3 draws=9800 passes=162 dispatches=1100 new_pipes=0 faults=1200 copy_mb=38 fp_probe_mb=2900 fp_host_mb=4600 swap_mb=0 pressure=0 therm=0 future_key=7 bad=x nan=nan"
    let w = DetailedBenchmark.parse(line: Substring(sample))
    check(w?["fps"] == 59.8 && w?["cpu.DrawRec"] == 62 && w?["fp_host_mb"] == 4600, "perf line parsed")
    check(w?["future_key"] == 7 && w?["bad"] == nil && w?["nan"] == nil, "unknown keys kept, junk dropped")
    check(DetailedBenchmark.parse(line: "Frame stats: 60.0 FPS, worst frame 20.0 ms") == nil, "not a perf line")
    check(DetailedBenchmark.parse(line: "Perf: ")?.values == nil, "empty perf line")
    check(DetailedBenchmark.parse(line: "Perf: fps=30")?["gpu_busy"] == nil, "missing keys stay missing")

    check(DetailedBenchmark.verdict(
            full: DetailedBenchmark.phase(log: (0..<10).map { _ in "Perf: fps=90 gpu_busy=70 cpu.game_unnamed=87 cpu.DrawRec=64" }.joined(separator: "\n"),
                                          measured: 8, renderScale: 100, duration: 90)!,
            half: DetailedBenchmark.phase(log: "Native rendering: output selection 1920x1080, scale 50%, scene/output surface 960x540, logical HUD 1920x1080\n"
                                              + (0..<8).map { _ in "Perf: fps=95 gpu_busy=60" }.joined(separator: "\n"),
                                          measured: 6, renderScale: 50, duration: 80),
            cap: 120).text.contains("busiest port thread DrawRec"), "busiest port thread skips the game's unnamed threads")
    func log(_ n: Int, fps: Double, gpu: Double?, drawRec: Double = 40, extra: String = "") -> String {
        (0..<n).map { i in
            "Perf: fps=\(fps + Double(i % 2)) ft_p99=\(20 + i) " + (gpu.map { "gpu_busy=\($0) " } ?? "")
                + "cpu.main=30 cpu.DrawRec=\(drawRec) fp_probe_mb=3000 fp_host_mb=5000 pressure=0 therm=\(i > 3 ? 1 : 0) swap_mb=0 \(extra)"
        }.joined(separator: "\n")
    }
    func phase(_ text: String, scale: Int = 100, measured: Int = 8) -> DetailedBenchmark.Phase? {
        // A run below 100% logs the native patch's surface, as the game does.
        let native = scale < 100
            ? "Native rendering: output selection 1920x1080, scale \(scale)%, scene/output surface \(1920 * scale / 100)x\(1080 * scale / 100), logical HUD 1920x1080\n"
            : ""
        return DetailedBenchmark.phase(log: native + text, measured: measured, renderScale: scale, duration: 90)
    }
    check(DetailedBenchmark.renderSurface(log: "x\nNative rendering: output selection 3840x2160, scale 50%, scene/output surface 1920x1080, logical HUD 3840x2160\n") == (1920, 1080)
          && DetailedBenchmark.renderSurface(log: "") == (1920, 1080), "render surface from the log")
    check(phase(log(9, fps: 50, gpu: 50)) == nil, "too few windows")
    let p = phase(log(12, fps: 50, gpu: 50))!
    check(p.windows.count == 8 && p.warmup.count == 4 && p.hasPerf, "last 8 windows measured")
    check(p.median("fps") == 50.5 && p.worst("ft_p99") == 31 && p.median("ft_p99") == 27.5, "median and worst")
    check(p.threads.first?.name == "DrawRec" || p.threads.first?.name == "main", "threads listed")

    // Verdicts.
    let gpuFull = phase(log(10, fps: 40, gpu: 97, drawRec: 30))!
    let gpuHalf = phase(log(8, fps: 75, gpu: 60), scale: 50, measured: 6)!
    var v = DetailedBenchmark.verdict(full: gpuFull, half: gpuHalf, cap: 120)
    let notHalved = phase(log(8, fps: 41, gpu: 97), scale: 100, measured: 6)!
    check(!DetailedBenchmark.verdict(full: gpuFull, half: notHalved, cap: 120).text.contains("half render scale"),
          "a half run that rendered the same size gives no gain")
    check(v.kind == .gpuBound && v.text.contains("GPU busy 97%") && v.text.contains("+"), "GPU-bound: \(v.text)")
    let cpuFull = phase(log(10, fps: 40, gpu: 55, drawRec: 92))!
    let cpuHalf = phase(log(8, fps: 41, gpu: 30), scale: 50, measured: 6)!
    v = DetailedBenchmark.verdict(full: cpuFull, half: cpuHalf, cap: 120)
    check(v.kind == .cpuBound && v.text.contains("busiest port thread DrawRec 92%") && v.text.contains("half render scale +"), "CPU-bound: \(v.text)")
    let capFull = phase(log(10, fps: 59, gpu: 55))!
    v = DetailedBenchmark.verdict(full: capFull, half: cpuHalf, cap: 60)
    check(v.kind == .frameRateCap && v.text == "Frame rate cap (60 FPS held, GPU busy 55%)", "cap: \(v.text)")
    v = DetailedBenchmark.verdict(full: capFull, half: nil, cap: 70)
    check(v.kind != .frameRateCap, "58 FPS is not within 3% of 70")
    let mixHalf = phase(log(8, fps: 50, gpu: 50), scale: 50, measured: 6)!
    v = DetailedBenchmark.verdict(full: phase(log(10, fps: 39, gpu: 80))!, half: mixHalf, cap: 120)
    check(v.kind == .mixed, "mixed (+25%): \(v.text)")
    v = DetailedBenchmark.verdict(full: phase(log(10, fps: 40, gpu: 95))!, half: nil, cap: 0)
    check(v.kind == .gpuBound, "GPU busy alone decides without a second run")
    v = DetailedBenchmark.verdict(full: phase(log(10, fps: 40, gpu: nil))!, half: nil, cap: 0)
    check(v.kind == .notEnoughData, "nothing to decide on")

    // Old game build: Frame stats only.
    let old = (0..<10).map { _ in "Frame stats: 45.0 FPS, worst frame 30.0 ms (x)" }.joined(separator: "\n")
    let oldPhase = phase(old)!
    check(!oldPhase.hasPerf && oldPhase.fps == 45 && oldPhase.median("ft_max") == 30, "frame stats fallback")
    v = DetailedBenchmark.verdict(full: oldPhase, half: nil, cap: 60)
    check(v.kind == .needsNewerBuild && v.text.contains("newer game build"), "old build: \(v.text)")

    // Memory and thermal lines.
    check(DetailedBenchmark.memoryLine(phases: [gpuFull], memoryGB: 16) == "Memory: 7.8 of 16 GB, pressure normal, no swap",
          "memory line: \(DetailedBenchmark.memoryLine(phases: [gpuFull], memoryGB: 16) ?? "nil")")
    check(DetailedBenchmark.thermalLine(phases: [gpuFull]) == "Thermal: nominal -> fair", "thermal line")
    check(DetailedBenchmark.memoryLine(phases: [oldPhase], memoryGB: 16) == nil, "no memory line without data")
    check(DetailedBenchmark.halfScale(of: .full) == 50 && DetailedBenchmark.halfScale(of: .p67) == 33
          && DetailedBenchmark.halfScale(of: .p50) == 25, "half scale")

    // Report: keys of both phases, nothing that identifies the owner.
    let system = SystemInfo(appVersion: "0.1.20", model: "Mac15,8", chip: "Apple M3 Max", memoryGB: 64, macOS: "Version 26.6 (Build 25G62)")
    let machine = DetailedBenchmark.Machine(system: system, display: "3456x2234 at 120 Hz",
                                            power: PowerState(mode: .automatic, onAC: true), lowPowerMode: false)
    let report = DetailedBenchmark.Report(full: gpuFull, half: gpuHalf, cap: 120, machine: machine,
                                          settings: ["full screen", "render scale 100%"], date: Date(timeIntervalSince1970: 0))
    let json = report.json
    let parsed = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any]
    let phases = parsed?["phases"] as? [String: Any]
    check(((phases?["yourSettings"] as? [String: Any])?["windows"] as? [Any])?.count == 8, "json: phase 1 windows")
    check(((phases?["halfRenderScale"] as? [String: Any])?["windows"] as? [Any])?.count == 6, "json: phase 2 windows")
    check((parsed?["mac"] as? [String: Any])?["chip"] as? String == "Apple M3 Max", "json: chip")
    let user = NSUserName(), home = NSHomeDirectory(), host = ProcessInfo.processInfo.hostName
    for (what, out) in [("json", json), ("text", report.text), ("issue", report.issueURL?.absoluteString ?? "")] {
        check(!out.contains(home), "\(what) has no home path")
        check(user.count < 3 || !out.contains(user), "\(what) has no user name")
        check(host.count < 3 || !out.contains(host), "\(what) has no host name")
        check(!out.localizedCaseInsensitiveContains("serial") && !out.contains("/Users/"), "\(what) has no serial or path")
    }
    check(report.text.contains("Verdict: GPU-bound") && report.text.contains("Memory: ") && report.text.contains("Thermal: "), "text summary")
    check(report.issueURL?.absoluteString.hasPrefix("https://github.com/satperm/iss_mac_port/issues/new") == true, "issue link")
}

if failures == 0 {
    print("PASS: launcher model tests")
} else {
    print("\(failures) FAILED")
    exit(1)
}
