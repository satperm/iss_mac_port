// SPDX-FileCopyrightText: Copyright 2026 satperm
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived in part from bloodborne_mac_port launcher/Model/Bindings.swift (Copyright 2026 Mark Hall).
import Foundation

/// A PS4 button the keyboard and mouse can press (src/runtime_pad.c, `ps_names` and `key_map`).
struct KeyButton: Identifiable, Hashable {
    /// The name TLG_KEY_MAP uses.
    let key: String
    let label: String
    /// The game's default sources, in the order of the `key_map` table in runtime_pad.c.
    let defaults: [String]
    var id: String { key }
}

/// A key or mouse input the game reads in a fixed way (not remappable).
struct FixedKey: Identifiable {
    let label: String
    let keys: String
    var id: String { label }
}

/// The keyboard and mouse bindings: the TLG_KEY_MAP variable, `cross=Space,triangle=Q|Mouse Left`
/// (PS button = one or more sources joined by `|`; `none` unbinds). A button that is not named
/// keeps its defaults; a named one loses them. Only buttons changed from the defaults are written,
/// and nothing at all when every button is default.
struct KeyMap: Equatable {
    static let variable = "TLG_KEY_MAP"
    /// Sources per button. The game takes up to 8; three keep the longest possible string
    /// (17 buttons) under the 1024-byte parser buffer of apply_button_map.
    static let slots = 3
    static let limit = 1000

    static let buttons: [KeyButton] = [
        KeyButton(key: "cross", label: "Cross", defaults: ["Space"]),
        KeyButton(key: "circle", label: "Circle", defaults: ["Left Shift"]),
        KeyButton(key: "square", label: "Square", defaults: ["E"]),
        KeyButton(key: "triangle", label: "Triangle", defaults: ["Q"]),
        KeyButton(key: "l1", label: "L1", defaults: ["1"]),
        KeyButton(key: "r1", label: "R1", defaults: ["3"]),
        KeyButton(key: "l2", label: "L2 (aim)", defaults: ["R", "Mouse Right"]),
        KeyButton(key: "r2", label: "R2 (use the power)", defaults: ["F", "Mouse Left"]),
        KeyButton(key: "l3", label: "L3", defaults: ["Z"]),
        KeyButton(key: "r3", label: "R3", defaults: ["C", "Mouse Middle"]),
        KeyButton(key: "options", label: "Options", defaults: ["Return"]),
        KeyButton(key: "touchpad_left", label: "Touchpad, left side (photo mode)", defaults: ["Tab"]),
        KeyButton(key: "touchpad_right", label: "Touchpad, right side (pause menu)", defaults: ["Backspace"]),
        KeyButton(key: "up", label: "D-pad up", defaults: ["I"]),
        KeyButton(key: "down", label: "D-pad down", defaults: ["K"]),
        KeyButton(key: "left", label: "D-pad left", defaults: ["J"]),
        KeyButton(key: "right", label: "D-pad right", defaults: ["L"]),
    ]

    /// The game also accepts `touchpad` for `touchpad_left`.
    static let aliases = ["touchpad": "touchpad_left"]

    static let mouseNames = ["Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2",
                             "Wheel Up", "Wheel Down", "Wheel Left", "Wheel Right"]
    /// Cmd belongs to the Mac (shortcuts), Esc gives the mouse back: neither is offered.
    static let reserved = ["Left GUI", "Right GUI", "Escape"]

    /// Read directly by the game: WASD, the arrow keys, T, G and Y (runtime_pad.c read_pad,
    /// sample_motion, apply_swipe), Esc (gpu/shim/window.cpp), mouse movement and the touchpad mode keys.
    static let fixed: [FixedKey] = [
        FixedKey(label: "Left stick", keys: "W A S D"),
        FixedKey(label: "Right stick", keys: "Arrow keys"),
        FixedKey(label: "Camera", keys: "Mouse movement"),
        FixedKey(label: "Spray can sideways (graffiti)", keys: "T / G"),
        FixedKey(label: "Shake the spray can", keys: "Y"),
        FixedKey(label: "Touchpad swipe, mouse touchpad mode", keys: "Tab + arrows / Left Ctrl or Mouse X1"),
        FixedKey(label: "Release the mouse", keys: "Esc"),
    ]
    /// The keys behind `fixed` that a button should not share.
    static let fixedKeys = ["W", "A", "S", "D", "Up", "Down", "Left", "Right", "T", "G", "Y", "Left Ctrl", "Mouse X1"]

    private var bound: [String: [String]]

    init() { bound = Dictionary(uniqueKeysWithValues: Self.buttons.map { ($0.key, $0.defaults) }) }

    /// Reads a TLG_KEY_MAP value the way the game does: entries split at `,`, names at `|`, names
    /// matched without regard to case; unknown buttons and names are dropped.
    init(string: String?) {
        self.init()
        guard let string else { return }
        for entry in string.split(separator: ",", omittingEmptySubsequences: true) {
            guard let eq = entry.firstIndex(of: "=") else { continue }
            let raw = entry[..<eq].trimmingCharacters(in: .whitespaces).lowercased()
            let key = Self.aliases[raw] ?? raw
            guard let button = Self.buttons.first(where: { $0.key == key }) else { continue }
            let value = entry[entry.index(after: eq)...].trimmingCharacters(in: .whitespaces)
            if value.caseInsensitiveCompare("none") == .orderedSame {
                bound[button.key] = []
                continue
            }
            let names = Self.parse(value)
            if !names.isEmpty { bound[button.key] = names } // all bad: the game keeps the default
        }
    }

    /// `A|B` as canonical names, unknown and reserved ones dropped, no repeats, at most `slots`.
    static func parse(_ value: String) -> [String] {
        var out: [String] = []
        for part in value.split(separator: "|") {
            if let name = canonical(part.trimmingCharacters(in: .whitespaces)), !out.contains(name) { out.append(name) }
        }
        return Array(out.prefix(slots))
    }

    /// The spelling SDL and the game use, or nil if the name is unknown or not offered.
    static func canonical(_ name: String) -> String? {
        guard let match = (mouseNames + KeyCodes.names).first(where: { $0.caseInsensitiveCompare(name) == .orderedSame }),
              !reserved.contains(match) else { return nil }
        return match
    }

    subscript(button: KeyButton) -> [String] {
        get { bound[button.key] ?? button.defaults }
        set { bound[button.key] = Array(newValue.prefix(Self.slots)) }
    }

    /// Puts `name` in slot `slot` of `button` (past the end: appended); nil removes the slot.
    mutating func set(_ button: KeyButton, slot: Int, to name: String?) {
        var names = self[button]
        if let name {
            if let same = names.firstIndex(of: name) { names.remove(at: same) }
            if slot < names.count { names[min(slot, names.count - 1)] = name } else { names.append(name) }
        } else if slot < names.count {
            names.remove(at: slot)
        }
        self[button] = names
    }

    mutating func reset(_ button: KeyButton) { bound[button.key] = button.defaults }

    func isDefault(_ button: KeyButton) -> Bool { Set(self[button]) == Set(button.defaults) }
    var isDefault: Bool { Self.buttons.allSatisfy(isDefault) }

    /// The value for TLG_KEY_MAP, or nil when everything is default.
    var string: String? {
        let parts = Self.buttons.filter { !isDefault($0) }.map { button -> String in
            let names = self[button]
            return "\(button.key)=\(names.isEmpty ? "none" : names.joined(separator: "|"))"
        }
        return parts.isEmpty ? nil : parts.joined(separator: ",")
    }

    /// The string stays below the game's 1024-byte buffer (it would cut it off silently).
    var fits: Bool { (string ?? "").utf8.count < Self.limit }

    /// The other buttons that use `name`: one key can press several buttons, the game allows it,
    /// so this is a warning only.
    func others(using name: String, besides button: KeyButton) -> [KeyButton] {
        Self.buttons.filter { $0 != button && self[$0].contains(name) }
    }

    /// Every key or mouse input used by two or more buttons.
    var conflicts: [(name: String, buttons: [KeyButton])] {
        var users: [String: [KeyButton]] = [:]
        var order: [String] = []
        for button in Self.buttons {
            for name in self[button] {
                if users[name] == nil { order.append(name) }
                users[name, default: []].append(button)
            }
        }
        return order.compactMap { users[$0]!.count > 1 ? ($0, users[$0]!) : nil }
    }

    /// What `name` already does outside the table, if anything.
    static func fixedUse(of name: String) -> String? {
        switch name {
        case "W", "A", "S", "D": "left stick"
        case "Up", "Down", "Left", "Right": "right stick (and aiming the spray can, touchpad swipe)"
        case "T", "G": "spray can held sideways"
        case "Y": "shaking the spray can"
        case "Left Ctrl", "Mouse X1": "touchpad mode of the mouse"
        default: nil
        }
    }
}
