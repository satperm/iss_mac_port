// SPDX-License-Identifier: GPL-2.0-or-later
// ISS launcher settings: port/launch.conf, shell assignments that run_macos.sh reads (the app
// sources it too). Lines the launcher does not know are kept as they are.
import Foundation

enum FramePacing: String, CaseIterable, Identifiable {
    case smooth = "0", vblank60 = "60"
    var id: String { rawValue }
    var label: String { self == .smooth ? "Smooth (uncapped)" : "60 Hz steps" }
}

/// The frame rate cap (TLG_FPS_LIMIT): the display's refresh rate (at most 120) unless a lower cap is
/// chosen. 30 suits slower Macs: steadier than an uneven 35-50.
enum FrameRateCap: String, CaseIterable, Identifiable {
    case display = "", fps60 = "60", fps30 = "30"
    var id: String { rawValue }
    var label: String {
        switch self {
        case .display: "Display"
        case .fps60: "60"
        case .fps30: "30"
        }
    }
}

/// The game's render size as a percentage of 1080p (TLG_RENDER_SCALE, the eboot's native resolution
/// patch): fewer pixels to shade on slower Macs; the picture is scaled up to the window.
enum RenderScale: String, CaseIterable, Identifiable {
    case full = "", p85 = "85", p75 = "75", p67 = "67", p50 = "50"
    var id: String { rawValue }
    var label: String { self == .full ? "100%" : "\(rawValue)%" }
}

/// How many sun-shadow cascades the game draws (TLG_SHADOW_CASCADES): Full leaves the variable unset;
/// fewer cascades mean far fewer draws in open scenes, with coarser or missing distant shadows.
enum ShadowDetail: String, CaseIterable, Identifiable {
    case full = "", lower = "-2", lowest = "-3"
    var id: String { rawValue }
    var label: String {
        switch self {
        case .full: "Full"
        case .lower: "Lower"
        case .lowest: "Lowest"
        }
    }
}

/// One frame counter at a time: the game's own FPS counter (TLG_SHOW_FPS) or Apple's Metal
/// Performance HUD (MTL_HUD_ENABLED).
enum FrameCounter: String, CaseIterable, Identifiable {
    case off, game, metal
    var id: String { rawValue }
    var label: String {
        switch self {
        case .off: "Off"
        case .game: "Game"
        case .metal: "Metal HUD"
        }
    }
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

/// One choice that sets detail, frame rate cap, render scale and GPU particles together. Slower Macs
/// (fewer GPU cores, 4 performance cores) are limited by the GPU first: fewer pixels and a 30 FPS cap
/// give a steady picture. Nothing is measured on such Macs yet: the values are a starting point.
enum PerformancePreset: String, CaseIterable, Identifiable {
    case standard, slower, slowest
    var id: String { rawValue }
    var label: String {
        switch self {
        case .standard: "Default"
        case .slower: "Slower Mac"
        case .slowest: "Slowest"
        }
    }
    var quality: QualityPreset {
        switch self {
        case .standard: .original
        case .slower: .low
        case .slowest: .lowest
        }
    }
    var fpsCap: FrameRateCap { self == .standard ? .display : .fps30 }
    var renderScale: RenderScale {
        switch self {
        case .standard: .full
        case .slower: .p67
        case .slowest: .p50
        }
    }
    var particles: ParticleMode { self == .slowest ? .off : .auto }
    var shadowDetail: ShadowDetail { .full }
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
    var fpsCap = FrameRateCap.display
    var renderScale = RenderScale.full
    var shadowDetail = ShadowDetail.full
    var particles = ParticleMode.auto
    var textLanguage = "1"
    var voiceLanguage = ""
    var gpuProcess = true

    /// Show FPS and the Metal HUD as one choice; a file with both on reads as the Metal HUD.
    var counter: FrameCounter {
        get { metalHUD ? .metal : showFPS ? .game : .off }
        set {
            showFPS = newValue == .game
            metalHUD = newValue == .metal
        }
    }

    /// The preset the four settings match, nil when they were changed one by one ("Custom").
    var preset: PerformancePreset? {
        get {
            PerformancePreset.allCases.first {
                $0.quality == quality && $0.fpsCap == fpsCap && $0.renderScale == renderScale && $0.particles == particles
                    && $0.shadowDetail == shadowDetail
            }
        }
        set {
            guard let p = newValue else { return }
            quality = p.quality
            fpsCap = p.fpsCap
            renderScale = p.renderScale
            particles = p.particles
            shadowDetail = p.shadowDetail
        }
    }

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
        if metalHUD { showFPS = false } // one counter at a time
        quality = value("TLG_PERF_MODE").flatMap(QualityPreset.init(rawValue:)) ?? .original
        fpsCap = value("TLG_FPS_LIMIT").flatMap(FrameRateCap.init(rawValue:)) ?? .display
        renderScale = value("TLG_RENDER_SCALE").flatMap(RenderScale.init(rawValue:)) ?? .full
        shadowDetail = value("TLG_SHADOW_CASCADES").flatMap(ShadowDetail.init(rawValue:)) ?? .full
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
        set("TLG_FPS_LIMIT", fpsCap == .display ? nil : fpsCap.rawValue)
        set("TLG_RENDER_SCALE", renderScale == .full ? nil : renderScale.rawValue)
        set("TLG_SHADOW_CASCADES", shadowDetail == .full ? nil : shadowDetail.rawValue)
        set("TLG_PARTICLES", particles == .auto ? nil : particles.rawValue)
        set("TLG_LANGUAGE", textLanguage)
        set("TLG_AUDIO_LANGUAGE", voiceLanguage.isEmpty ? nil : voiceLanguage)
        set("TLG_GPU_PROCESS", gpuProcess ? nil : "0")
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
         "quality \(quality.label)", "FPS cap \(fpsCap.label)", "render scale \(renderScale.label)", "shadow detail \(shadowDetail.label)",
         "particles \(particles.label)", gpuProcess ? "GPU process" : "single process"]
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
