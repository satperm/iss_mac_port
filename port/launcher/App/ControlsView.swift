// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-FileCopyrightText: Copyright 2026 satperm
// SPDX-License-Identifier: GPL-2.0-or-later
// Derived in part from bloodborne_mac_port launcher/App/ControlsView.swift.
import AppKit
import Carbon.HIToolbox
import SwiftUI

/// The launcher's Controls page: which keys and mouse buttons press which PS4 button
/// (Model/KeyMap.swift; the game reads TLG_KEY_MAP at start, src/runtime_pad.c).
struct ControlsView: View {
    @ObservedObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss
    /// The slot waiting for a key or mouse button.
    @State private var capturing: Capture?

    struct Capture: Identifiable {
        let button: KeyButton
        let slot: Int
        var id: String { "\(button.key)/\(slot)" }
    }

    private var keys: KeyMap { model.settings.keys }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Controls").font(.title2.bold())
            Text("Click a key to change it, then press the new key or mouse button. A button you change loses its default keys.")
                .font(.callout).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if model.running {
                Text("The game is running: changes apply the next time it starts.")
                    .font(.caption).foregroundStyle(.orange)
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    table
                    fixedTable
                    helpBox(keys.sprayHelp(label: KeyLabel.text))
                    helpBox(keys.touchpadHelp(label: KeyLabel.text))
                }
                .padding(.trailing, 8)
            }
            if !keys.fits {
                Text("Too many custom keys to pass to the game; remove some.")
                    .font(.caption).foregroundStyle(.red)
            }
            HStack {
                Button("Reset All") { model.settings.keys = KeyMap() }
                    .disabled(keys.isDefault)
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 700, height: 540)
        .sheet(item: $capturing) { capture in
            let names = keys[capture.button]
            CaptureSheet(button: capture.button,
                         current: capture.slot < names.count ? names[capture.slot] : nil,
                         keys: keys, slot: capture.slot) { name in
                var next = keys
                next.set(capture.button, slot: capture.slot, to: name)
                if next.fits { model.settings.keys = next }
                capturing = nil
            } cancel: {
                capturing = nil
            }
        }
    }

    private var table: some View {
        Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 4) {
            GridRow {
                Text("Action").font(.caption.bold()).foregroundStyle(.secondary)
                Text("Keys").font(.caption.bold()).foregroundStyle(.secondary)
                Text("")
            }
            Divider().gridCellColumns(3)
            ForEach(KeyMap.buttons) { button in
                row(button)
            }
            Divider().gridCellColumns(3)
            ForEach(KeyMap.actions) { button in
                row(button)
            }
        }
    }

    private func row(_ button: KeyButton) -> some View {
        let names = keys[button]
        let shared = names.filter { !keys.others(using: $0, besides: button).isEmpty || KeyMap.fixedUse(of: $0) != nil }
        return GridRow {
            Text(button.label).frame(width: 230, alignment: .leading)
            HStack(spacing: 4) {
                ForEach(0..<names.count, id: \.self) { slot in
                    Button { capturing = Capture(button: button, slot: slot) } label: {
                        Text(KeyLabel.text(names[slot])).lineLimit(1).frame(minWidth: 64)
                    }
                    .help("\(names[slot]): click to change")
                }
                if names.count < KeyMap.slots {
                    Button { capturing = Capture(button: button, slot: names.count) } label: {
                        Text(names.isEmpty ? "None (click to set)" : "+").frame(minWidth: names.isEmpty ? 64 : 12)
                            .foregroundStyle(.secondary)
                    }
                    .help("Click, then press a key or mouse button")
                }
                if !shared.isEmpty {
                    Image(systemName: "exclamationmark.triangle").foregroundStyle(.orange)
                        .help(conflictHelp(button, shared))
                }
            }
            HStack(spacing: 4) {
                Button("Clear") { model.settings.keys[button] = [] }
                    .controlSize(.small).disabled(names.isEmpty)
                Button("Default") { model.settings.keys.reset(button) }
                    .controlSize(.small).disabled(keys.isDefault(button))
            }
        }
    }

    private func conflictHelp(_ button: KeyButton, _ names: [String]) -> String {
        names.map { name in
            let others = keys.others(using: name, besides: button).map(\.label)
            let fixed = KeyMap.fixedUse(of: name).map { [$0] } ?? []
            return "\(name) also does: " + (others + fixed).joined(separator: ", ")
        }.joined(separator: "; ")
    }

    private var fixedTable: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("Fixed").font(.caption.bold()).foregroundStyle(.secondary)
            Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 3) {
                ForEach(KeyMap.fixed) { item in
                    GridRow {
                        Text(item.label).frame(width: 230, alignment: .leading)
                        Text(item.keys)
                    }
                    .foregroundStyle(.secondary)
                }
            }
        }
    }

    private func helpBox(_ text: String) -> some View {
        Text(text)
            .font(.callout)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(10)
            .background(RoundedRectangle(cornerRadius: 8).fill(.quaternary))
    }
}

/// Waits for one key, mouse button or wheel turn in its box. Esc cancels; Clear removes the slot.
private struct CaptureSheet: View {
    let button: KeyButton
    let current: String?
    let keys: KeyMap
    let slot: Int
    let done: (String?) -> Void
    let cancel: () -> Void
    @State private var message: String?
    @State private var monitor: Any?

    var body: some View {
        VStack(spacing: 14) {
            Text(button.label).font(.headline)
            CaptureArea { name in done(name) }
                .frame(width: 360, height: 110)
                .overlay {
                    VStack(spacing: 6) {
                        Text("Press a key, click a mouse button or turn the wheel here")
                        if let current { Text("Now: \(KeyLabel.text(current))").foregroundStyle(.secondary) }
                        Text("Esc cancels").font(.caption).foregroundStyle(.secondary)
                    }
                    .allowsHitTesting(false)
                }
                .background(RoundedRectangle(cornerRadius: 8).fill(.quaternary))
            if let message { Text(message).font(.caption).foregroundStyle(.orange) }
            HStack {
                Button("Clear") { done(nil) }.disabled(current == nil)
                Spacer()
                Button("Cancel", action: cancel)
            }
        }
        .padding(20)
        .frame(width: 400)
        .onAppear {
            monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .flagsChanged]) { event in
                if event.type == .keyDown && Int(event.keyCode) == kVK_Escape { cancel(); return nil }
                switch KeyLabel.name(for: event) {
                case .bound(let name): done(name)
                case .refused(let why): message = why
                case .ignored: break
                }
                return nil
            }
        }
        .onDisappear {
            if let monitor { NSEvent.removeMonitor(monitor) }
            monitor = nil
        }
    }
}

/// The box that takes mouse buttons and the wheel.
private struct CaptureArea: NSViewRepresentable {
    let bound: (String) -> Void

    final class View: NSView {
        var bound: (String) -> Void = { _ in }
        private var wheelY = 0.0, wheelX = 0.0
        override func mouseDown(with event: NSEvent) { bound("Mouse Left") }
        override func rightMouseDown(with event: NSEvent) { bound("Mouse Right") }
        override func otherMouseDown(with event: NSEvent) {
            // NSEvent numbers: 2 middle, 3 back, 4 forward (SDL's X1 and X2).
            switch event.buttonNumber {
            case 2: bound("Mouse Middle")
            case 3: bound("Mouse X1")
            case 4: bound("Mouse X2")
            default: break
            }
        }
        override func scrollWheel(with event: NSEvent) {
            // The physical direction, not "natural scrolling".
            let flip: Double = event.isDirectionInvertedFromDevice ? -1 : 1
            let scale = event.hasPreciseScrollingDeltas ? 10.0 : 1.0
            wheelY += flip * event.scrollingDeltaY / scale
            wheelX += flip * event.scrollingDeltaX / scale
            if wheelY >= 1 { bound("Wheel Up") } else if wheelY <= -1 { bound("Wheel Down") }
            else if wheelX >= 1 { bound("Wheel Left") } else if wheelX <= -1 { bound("Wheel Right") }
        }
    }

    func makeNSView(context: Context) -> View { let v = View(); v.bound = bound; return v }
    func updateNSView(_ view: View, context: Context) { view.bound = bound }
}

/// Names for keys: SDL's names (what TLG_KEY_MAP takes) and what the launcher shows.
enum KeyLabel {
    enum Result { case bound(String), refused(String), ignored }

    /// A key press (keyDown, or a modifier going down in flagsChanged) as a binding.
    static func name(for event: NSEvent) -> Result {
        if event.type == .flagsChanged {
            let flag: NSEvent.ModifierFlags? = switch Int(event.keyCode) {
            case kVK_Shift, kVK_RightShift: .shift
            case kVK_Option, kVK_RightOption: .option
            case kVK_Control, kVK_RightControl: .control
            case kVK_Command, kVK_RightCommand: .command
            case kVK_CapsLock: nil // each press toggles it: take either
            default: .function
            }
            if let flag, !event.modifierFlags.contains(flag) { return .ignored } // released
        }
        guard let name = KeyCodes.name(keyCode: event.keyCode) else {
            return .refused("The game can't read that key.")
        }
        if KeyMap.reserved.contains(name) { return .refused("\u{2318} is kept for Mac shortcuts.") }
        return .bound(name)
    }

    private static let fixed: [String: String] = [
        "Mouse Left": "Left click", "Mouse Right": "Right click", "Mouse Middle": "Middle click",
        "Mouse X1": "Mouse X1 (back)", "Mouse X2": "Mouse X2 (forward)",
        "Wheel Up": "Wheel up", "Wheel Down": "Wheel down", "Wheel Left": "Wheel left", "Wheel Right": "Wheel right",
        "Left Shift": "\u{21E7} Left", "Right Shift": "\u{21E7} Right",
        "Left Alt": "\u{2325} Left", "Right Alt": "\u{2325} Right",
        "Left Ctrl": "\u{2303} Left", "Right Ctrl": "\u{2303} Right",
        "Up": "\u{2191}", "Down": "\u{2193}", "Left": "\u{2190}", "Right": "\u{2192}",
        "Backspace": "Delete \u{232B}", "Delete": "Fwd Del \u{2326}", "Insert": "Help", "Keypad Enter": "Enter",
    ]

    /// What is printed on the key in the current keyboard layout (letters, digits, symbols), else
    /// a readable name.
    static func text(_ name: String) -> String {
        if let label = fixed[name] { return label }
        if name.count == 1, let code = KeyCodes.keyCode(name: name), let typed = typed(code) { return typed }
        return name
    }

    private static func typed(_ code: UInt16) -> String? {
        guard let source = TISCopyCurrentKeyboardLayoutInputSource()?.takeRetainedValue(),
              let raw = TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData) else { return nil }
        let data = Unmanaged<CFData>.fromOpaque(raw).takeUnretainedValue() as Data
        return data.withUnsafeBytes { bytes -> String? in
            guard let layout = bytes.baseAddress?.assumingMemoryBound(to: UCKeyboardLayout.self) else { return nil }
            var dead: UInt32 = 0
            var chars = [UniChar](repeating: 0, count: 4)
            var length = 0
            let status = UCKeyTranslate(layout, code, UInt16(kUCKeyActionDisplay), 0, UInt32(LMGetKbdType()),
                                        OptionBits(kUCKeyTranslateNoDeadKeysBit), &dead, chars.count, &length, &chars)
            guard status == noErr, length > 0 else { return nil }
            let s = String(utf16CodeUnits: chars, count: length).uppercased()
            return s.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : s
        }
    }
}
