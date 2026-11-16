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

/// The keyboard and mouse bindings (buttons and actions): the TLG_KEY_MAP variable, `cross=Space,triangle=Q|Mouse Left`
/// (PS button = one or more sources joined by `|`; `none` unbinds). A button that is not named
/// keeps its defaults; a named one loses them. Only buttons changed from the defaults are written,
/// and nothing at all when every button is default.
struct KeyMap: Equatable {
    static let variable = "TLG_KEY_MAP"
    /// Sources per button. The game takes up to 8; three keep the longest possible string
    /// (29 names) far under the 4096-byte parser buffer of apply_button_map.
    static let slots = 3
    static let limit = 4000

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

    /// Keyboard actions (src/runtime_pad.c `action_names`): the sticks, the spray can and the swipe
    /// modifier. `swipe` follows the left touchpad click's keys until it is set (the launcher keeps
    /// it out of the value then, like the game).
    static let actions: [KeyButton] = [
        KeyButton(key: "move_up", label: "Move up (left stick)", defaults: ["W"]),
        KeyButton(key: "move_down", label: "Move down (left stick)", defaults: ["S"]),
        KeyButton(key: "move_left", label: "Move left (left stick)", defaults: ["A"]),
        KeyButton(key: "move_right", label: "Move right (left stick)", defaults: ["D"]),
        KeyButton(key: "look_up", label: "Camera up (right stick)", defaults: ["Up"]),
        KeyButton(key: "look_down", label: "Camera down (right stick)", defaults: ["Down"]),
        KeyButton(key: "look_left", label: "Camera left (right stick)", defaults: ["Left"]),
        KeyButton(key: "look_right", label: "Camera right (right stick)", defaults: ["Right"]),
        KeyButton(key: "can_left", label: "Spray can sideways, one way (graffiti)", defaults: ["T"]),
        KeyButton(key: "can_right", label: "Spray can sideways, other way", defaults: ["G"]),
        KeyButton(key: "can_shake", label: "Shake the spray can", defaults: ["Y"]),
        KeyButton(key: "swipe", label: "Touchpad swipe modifier (+ camera keys)", defaults: ["Tab"]),
    ]
    static let swipe = actions[11]
    static let touchpadLeft = buttons[11]
    static let all = buttons + actions

    /// The game also accepts `touchpad` for `touchpad_left`.
    static let aliases = ["touchpad": "touchpad_left"]

    static let mouseNames = ["Mouse Left", "Mouse Middle", "Mouse Right", "Mouse X1", "Mouse X2",
                             "Wheel Up", "Wheel Down", "Wheel Left", "Wheel Right"]
    /// Cmd belongs to the Mac (shortcuts), Esc gives the mouse back, and the comma separates the
    /// entries of the map string (the game would read "cross=," as an empty binding): none is offered.
    static let reserved = ["Left GUI", "Right GUI", "Escape", ","]

    /// Read directly by the game: Esc (gpu/shim/window.cpp), mouse movement and the touchpad mode keys.
    static let fixed: [FixedKey] = [
        FixedKey(label: "Camera", keys: "Mouse movement"),
        FixedKey(label: "Touchpad mouse mode (hold)", keys: "Left Ctrl or Mouse X1"),
        FixedKey(label: "Release the mouse", keys: "Esc"),
    ]

    private var bound: [String: [String]]

    /// `swipe` has no entry until it is set: then it follows the left touchpad click.
    init() { bound = Dictionary(uniqueKeysWithValues: Self.all.filter { $0 != Self.swipe }.map { ($0.key, $0.defaults) }) }

    /// Reads a TLG_KEY_MAP value the way the game does: entries split at `,`, names at `|`, names
    /// matched without regard to case; unknown buttons and names are dropped.
    init(string: String?) {
        self.init()
        guard let string else { return }
        for entry in string.split(separator: ",", omittingEmptySubsequences: true) {
            guard let eq = entry.firstIndex(of: "=") else { continue }
            let raw = entry[..<eq].trimmingCharacters(in: .whitespaces).lowercased()
            let key = Self.aliases[raw] ?? raw
            guard let button = Self.all.first(where: { $0.key == key }) else { continue }
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
        get { bound[button.key] ?? (button == Self.swipe ? self[Self.touchpadLeft] : button.defaults) }
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

    mutating func reset(_ button: KeyButton) { bound[button.key] = button == Self.swipe ? nil : button.defaults }

    func isDefault(_ button: KeyButton) -> Bool {
        button == Self.swipe ? bound[button.key] == nil : Set(self[button]) == Set(button.defaults)
    }
    var isDefault: Bool { Self.all.allSatisfy(isDefault) }

    /// The value for TLG_KEY_MAP, or nil when everything is default.
    var string: String? {
        let parts = Self.all.filter { !isDefault($0) }.map { button -> String in
            let names = self[button]
            return "\(button.key)=\(names.isEmpty ? "none" : names.joined(separator: "|"))"
        }
        return parts.isEmpty ? nil : parts.joined(separator: ",")
    }

    /// The string stays below the game's 4096-byte buffer (it would cut it off silently).
    var fits: Bool { (string ?? "").utf8.count < Self.limit }

    /// The other buttons that use `name`: one key can press several buttons, the game allows it,
    /// so this is a warning only.
    func others(using name: String, besides button: KeyButton) -> [KeyButton] {
        Self.all.filter { $0 != button && self[$0].contains(name) && !sharesByDesign($0, button) }
    }

    /// Every key or mouse input used by two or more buttons.
    var conflicts: [(name: String, buttons: [KeyButton])] {
        var users: [String: [KeyButton]] = [:]
        var order: [String] = []
        for button in Self.all {
            for name in self[button] {
                if users[name] == nil { order.append(name) }
                users[name, default: []].append(button)
            }
        }
        return order.compactMap { name in
            let list = users[name]!
            let clash = list.indices.contains { a in list.indices.contains { b in a < b && !sharesByDesign(list[a], list[b]) } }
            return clash ? (name, list) : nil
        }
    }

    /// The swipe modifier is meant to be the left touchpad click's key (the default), so those two
    /// sharing a key is no conflict.
    private func sharesByDesign(_ a: KeyButton, _ b: KeyButton) -> Bool {
        (a == Self.swipe && b == Self.touchpadLeft) || (b == Self.swipe && a == Self.touchpadLeft)
    }

    /// What `name` already does outside the table, if anything.
    static func fixedUse(of name: String) -> String? {
        switch name {
        case "Left Ctrl", "Mouse X1": "touchpad mode of the mouse"
        default: nil
        }
    }

    /// `A / B` for a button's keys, `label` turning an SDL name into what the page shows.
    private func keysText(_ button: KeyButton, _ label: (String) -> String) -> String {
        let names = self[button]
        return names.isEmpty ? "(no key)" : names.map(label).joined(separator: " or ")
    }

    /// The spray can help box: names the current keys, the original wording when unchanged.
    func sprayHelp(label: (String) -> String = { $0 }) -> String {
        let used = ["can_left", "can_right", "look_up", "look_down", "look_left", "look_right", "can_shake"]
        let unchanged = used.allSatisfy { key in isDefault(Self.all.first { $0.key == key }!) } && isDefault(Self.buttons[7])
        if unchanged {
            return "Spray can without a gamepad: hold T or G to hold the controller sideways. Move the arrow keys or the mouse to aim. Press Y, or wiggle the mouse or spin the wheel, to shake the can. Hold F or the left mouse button to spray."
        }
        func act(_ key: String) -> String { keysText(Self.all.first { $0.key == key }!, label) }
        let aim = ["look_up", "look_down", "look_left", "look_right"].map(act).joined(separator: " / ")
        return "Spray can without a gamepad: hold \(act("can_left")) or \(act("can_right")) to hold the controller sideways. Move \(aim) or the mouse to aim. Press \(act("can_shake")), or wiggle the mouse or spin the wheel, to shake the can. Hold \(keysText(Self.buttons[7], label)) to spray."
    }

    /// The touchpad help box: the current click and swipe keys.
    func touchpadHelp(label: (String) -> String = { $0 }) -> String {
        let unchanged = isDefault(Self.touchpadLeft) && isDefault(Self.buttons[12]) && isDefault(Self.swipe)
            && ["look_up", "look_down", "look_left", "look_right"].allSatisfy { key in isDefault(Self.actions.first { $0.key == key }!) }
        if unchanged {
            return "Touchpad: Tab and Backspace click the left and right side of the touchpad. To swipe (for example Open Door), hold Tab and press an arrow key, or hold Left Ctrl (or Mouse X1) and drag with the mouse."
        }
        let aim = ["look_up", "look_down", "look_left", "look_right"].map { key in keysText(Self.actions.first { $0.key == key }!, label) }.joined(separator: " / ")
        return "Touchpad: \(keysText(Self.touchpadLeft, label)) and \(keysText(Self.buttons[12], label)) click the left and right side of the touchpad. To swipe (for example Open Door), hold \(keysText(Self.swipe, label)) and press a camera key (\(aim)), or hold Left Ctrl (or Mouse X1) and drag with the mouse."
    }
}
