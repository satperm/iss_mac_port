// SPDX-License-Identifier: GPL-2.0-or-later
// ISS launcher settings: port/launch.conf, shell assignments that run_macos.sh reads (the app
// sources it too). Lines the launcher does not know are kept as they are.
import Foundation

enum FramePacing: String, CaseIterable, Identifiable {
    case smooth = "0", vblank60 = "60"
    var id: String { rawValue }
    var label: String { self == .smooth ? "Smooth (uncapped)" : "60 Hz steps" }
}

enum QualityPreset: String, CaseIterable, Identifiable {
    case original = "", low = "low", lowest = "office"
    var id: String { rawValue }
    var label: String {
        switch self {
        case .original: "Original"
        case .low: "Lower"
        case .lowest: "Lowest"
        }
    }
}

/// How the game gets the results of its GPU particle simulation (smoke, sparks, neon, rain): it
/// reads survivor counts back from GPU memory every frame. Shared: the GPU writes straight into
/// the game's memory (TLG_UNIFIED_MEMORY) and the game reads the counts one frame later
/// (TLG_PARTICLE_LATE_READ). Precise: copies, read back on demand (TLG_READBACKS=2, as the Windows
/// port; slow). Off: Relaxed readbacks, the game never sees the counts and draws no GPU particles.
enum ParticleMode: String, CaseIterable, Identifiable {
    case auto = "", off = "off", precise = "precise", shared = "shared"
    var id: String { rawValue }
    var label: String {
        switch self {
        case .auto: "Auto"
        case .off: "Off"
        case .precise: "All particles"
        case .shared: "Shared memory"
        }
    }
}

/// Text: the PS4 system language (TLG_LANGUAGE), which also picks the voices unless a voice
/// language is chosen (TLG_AUDIO_LANGUAGE: the game's art/cache/lang_<name>_audio packs).
enum GameLanguage {
    static let text: [(code: String, label: String)] = [
        ("1", "English (US)"), ("18", "English (UK)"), ("8", "Russian"), ("2", "French"),
        ("22", "French (Canada)"), ("4", "German"), ("5", "Italian"), ("3", "Spanish"),
        ("20", "Spanish (Latin America)"), ("7", "Portuguese"), ("17", "Portuguese (Brazil)"),
        ("16", "Polish"), ("6", "Dutch"), ("19", "Turkish"), ("14", "Danish"), ("12", "Finnish"),
        ("15", "Norwegian"), ("13", "Swedish"),
    ]
    static let voices: [(name: String, label: String)] = [
        ("", "Same as text"), ("english", "English"), ("russian", "Russian"), ("french", "French"),
        ("german", "German"), ("italian", "Italian"), ("spanish", "Spanish"), ("portuguese", "Portuguese"),
        ("polish", "Polish"), ("dutch", "Dutch"), ("turkish", "Turkish"),
    ]
}

struct LaunchSettings: Equatable {
    var gameDir: URL?
    var showLauncher = true
    var fullscreen = true
    var stretch = true
    var pacing = FramePacing.smooth
    var showFPS = true
    var metalHUD = false
    var quality = QualityPreset.original
    var particles = ParticleMode.auto
    var textLanguage = "1"
    var voiceLanguage = ""
    var gpuProcess = true

    private var ini = IniFile()

    init() {}

    init(contentsOf url: URL) {
        ini = IniFile(contentsOf: url)
        func value(_ key: String) -> String? { ini[key].map(Shell.unquote) }
        func flag(_ key: String, _ fallback: Bool) -> Bool { value(key).map { $0 != "0" } ?? fallback }
        if let path = value("TLG_GAME_DIR"), !path.isEmpty { gameDir = URL(fileURLWithPath: path) }
        showLauncher = flag("TLG_SHOW_LAUNCHER", true)
        fullscreen = flag("TLG_FULLSCREEN", true)
        stretch = flag("TLG_STRETCH", true)
        pacing = value("TLG_VBLANK_HZ").flatMap(FramePacing.init(rawValue:)) ?? .smooth
        showFPS = flag("TLG_SHOW_FPS", true)
        metalHUD = flag("MTL_HUD_ENABLED", false)
        quality = value("TLG_PERF_MODE").flatMap(QualityPreset.init(rawValue:)) ?? .original
        textLanguage = value("TLG_LANGUAGE").flatMap { code in GameLanguage.text.contains { $0.code == code } ? code : nil } ?? "1"
        voiceLanguage = value("TLG_AUDIO_LANGUAGE") ?? ""
        particles = value("TLG_PARTICLES").flatMap(ParticleMode.init(rawValue:)) ?? .auto
        gpuProcess = flag("TLG_GPU_PROCESS", true)
    }

    /// The file's text with the settings written in.
    var text: String {
        var out = ini
        func set(_ key: String, _ value: String?) { out[key] = value.map(Shell.quote) }
        set("TLG_GAME_DIR", gameDir?.path)
        set("TLG_SHOW_LAUNCHER", showLauncher ? "1" : "0")
        set("TLG_FULLSCREEN", fullscreen ? "1" : "0")
        set("TLG_STRETCH", stretch ? "1" : "0")
        set("TLG_VBLANK_HZ", pacing.rawValue)
        set("TLG_SHOW_FPS", showFPS ? "1" : "0")
        set("MTL_HUD_ENABLED", metalHUD ? "1" : "0")
        set("TLG_PERF_MODE", quality == .original ? nil : quality.rawValue)
        set("TLG_PARTICLES", particles == .auto ? nil : particles.rawValue)
        set("TLG_LANGUAGE", textLanguage)
        set("TLG_AUDIO_LANGUAGE", voiceLanguage.isEmpty ? nil : voiceLanguage)
        set("TLG_GPU_PROCESS", gpuProcess ? "1" : "0")
        return out.text
    }

    func write(to url: URL) throws {
        try text.write(to: url, atomically: true, encoding: .utf8)
    }

    /// Every assignment of the file (the settings above and any other TLG_/MTL_/MVK_ lines).
    var environment: [String: String] {
        var env: [String: String] = [:]
        for case let .entry(key, value) in IniFile(text: text).lines { env[key] = Shell.unquote(value) }
        return env
    }

    /// For the session log and crash reports.
    var summary: [String] {
        [fullscreen ? "full screen" : "windowed", stretch ? "stretched" : "16:9", "pacing \(pacing.label)",
         "quality \(quality.label)", "particles \(particles.label)", gpuProcess ? "GPU process" : "single process"]
    }

    static func == (a: LaunchSettings, b: LaunchSettings) -> Bool { a.text == b.text }
}

enum Shell {
    /// Single-quoted for bash: it's -> 'it'\''s'.
    static func quote(_ s: String) -> String {
        "'" + s.replacingOccurrences(of: "'", with: "'\\''") + "'"
    }
    /// The value of a shell word: single quotes, double quotes and backslash escapes.
    static func unquote(_ s: String) -> String {
        var out = ""
        var single = false, double = false, escaped = false
        for c in s {
            if escaped {
                out.append(c)
                escaped = false
            } else if single {
                if c == "'" { single = false } else { out.append(c) }
            } else if c == "\\" {
                escaped = true
            } else if c == "'" && !double {
                single = true
            } else if c == "\"" {
                double.toggle()
            } else {
                out.append(c)
            }
        }
        return out
    }
}
