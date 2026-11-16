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
func button(_ key: String) -> KeyButton { KeyMap.buttons.first { $0.key == key }! }

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
    check(KeyMap.fixedUse(of: "W") != nil && KeyMap.fixedUse(of: "Space") == nil, "fixed keys")
}

// The longest possible value stays under the game's 1024-byte buffer.
do {
    var map = KeyMap()
    for b in KeyMap.buttons { map[b] = ["Keypad Enter", "Keypad .", "NonUSBackslash", "Right Shift"] }
    check(map[button("cross")].count == KeyMap.slots, "slots capped")
    check(map.fits && (map.string ?? "").utf8.count < 1024, "longest string \((map.string ?? "").utf8.count) bytes")
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
        let scancodes = ["SPACE": "Space", "LSHIFT": "Left Shift", "RETURN": "Return", "TAB": "Tab", "BACKSPACE": "Backspace"]
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
            let key: String? = ps == "CLICK_LEFT" ? "touchpad_left" : ps == "CLICK_RIGHT" ? "touchpad_right"
                : buttons[String(ps.dropFirst("BTN_".count))]
            if let name, let key { fromC[key, default: []].append(name); entries += 1 } else { check(false, "unparsed key_map entry \(src),\(ps)") }
        }
        check(entries == 20, "key_map has \(entries) entries (20 expected)")
        check(Set(fromC.keys) == Set(KeyMap.buttons.map(\.key)), "same buttons: \(fromC.keys.sorted())")
        for b in KeyMap.buttons { check(fromC[b.key] == b.defaults, "\(b.key): C \(fromC[b.key] ?? []) vs Swift \(b.defaults)") }
        // ps_names: every Swift button is a name the game parses.
        for b in KeyMap.buttons { check(c.contains("{\"\(b.key)\","), "ps_names has \(b.key)") }
        for (alias, target) in KeyMap.aliases { check(c.contains("{\"\(alias)\",CLICK_LEFT}") && target == "touchpad_left", "alias \(alias)") }
    } else {
        check(false, "cannot read src/runtime_pad.c (run from port/)")
    }
}

if failures == 0 {
    print("PASS: launcher model tests")
} else {
    print("\(failures) FAILED")
    exit(1)
}
