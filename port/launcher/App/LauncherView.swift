// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher window (layout as bloodborne_mac_port's: two pages of two columns, Play at the bottom).
import AppKit
import SwiftUI

struct LauncherView: View {
    static let size = CGSize(width: 860, height: 640)

    @ObservedObject var model: LauncherModel

    private enum Page: Hashable { case general, advanced }
    @State private var page = Page.general

    var body: some View {
        VStack(spacing: 0) {
            // Not a TabView: see bloodborne_mac_port #47 (SwiftUI loop when switching tabs back).
            Picker("", selection: $page) {
                Text("General").tag(Page.general)
                Text("Advanced").tag(Page.advanced)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()
            .padding(.top, 10)
            Group {
                switch page {
                case .general: general
                case .advanced: advanced
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .disabled(model.running)
            .onChange(of: model.settings) { model.save() }
            Divider()
            HStack(spacing: 12) {
                Toggle("Show launcher every time", isOn: $model.settings.showLauncher)
                    .disabled(model.running)
                    .help("Off: the app starts the game at once. Hold \u{2325} (Option) while opening it to see this window.")
                Button("Saves…") { model.showingSaves = true }
                Button("Logs") { model.openLogsFolder() }
                Button("Benchmark") { model.runBenchmark() }
                    .disabled(!model.canBenchmark)
                    .help("90 s at the truck crash (fire, smoke, sparks), standing still, with these settings; your saves are not touched. Results also go to Logs/Benchmarks.")
                Spacer()
                if let message = model.exitMessage {
                    Text(message).foregroundStyle(.red).lineLimit(2)
                    if model.crashReport != nil {
                        Button("Crash Report…") { model.showingCrashReport = true }
                    } else {
                        Button("Show Log", action: model.showLog)
                    }
                }
                if let hint = model.powerHint, !model.running, model.exitMessage == nil {
                    Text(hint).font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    Button("Battery Settings…") { model.openBatterySettings() }
                        .controlSize(.small)
                }
                if model.running {
                    Text(model.benchmarking ? "Benchmark running (90 s), don't touch the controls…"
                         : model.stopping ? "Stopping…" : "Game running").foregroundStyle(.secondary)
                    if model.preparingShaders && !model.stopping {
                        Text("First start: preparing shaders from your game files, once. The game window opens in about a minute, then compiles for up to a few minutes.")
                            .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                    }
                    Button(action: { model.stop() }) {
                        Label("Stop", systemImage: "stop.fill").frame(minWidth: 80)
                    }
                    .controlSize(.large)
                    .disabled(model.stopping)
                    .help("Close the running game. Save in the game first.")
                } else {
                    Button(action: model.play) {
                        Text("Play").frame(minWidth: 80)
                    }
                    .keyboardShortcut(.defaultAction)
                    .controlSize(.large)
                    .disabled(!model.canPlay)
                }
            }
            .padding()
        }
        .frame(width: LauncherView.size.width, height: LauncherView.size.height)
        .alert("Benchmark", isPresented: Binding(get: { model.benchmarkReport != nil },
                                                set: { if !$0 { model.benchmarkReport = nil } })) {
            Button("Copy") {
                NSPasteboard.general.clearContents()
                NSPasteboard.general.setString(model.benchmarkReport ?? "", forType: .string)
                model.benchmarkReport = nil
            }
            Button("Share on GitHub") { model.shareBenchmark() }
            Button("OK", role: .cancel) { model.benchmarkReport = nil }
        } message: {
            Text(model.benchmarkReport ?? "")
        }
        .sheet(isPresented: $model.showingCrashReport) {
            if let report = model.crashReport {
                CrashReportView(report: report, model: model)
            }
        }
        // (A second .sheet on the same view would never show: SwiftUI presents one per view.)
        .background {
            Color.clear.sheet(isPresented: $model.showingSaves) { SavesView(model: model) }
        }
    }

    private var general: some View {
        HStack(alignment: .top, spacing: 0) {
            Form {
                Section("Display") {
                    Picker("Window", selection: $model.settings.fullscreen) {
                        Text("Windowed").tag(false)
                        Text("Full screen").tag(true)
                    }
                    .pickerStyle(.segmented)
                    Toggle("Stretch to the display", isOn: $model.settings.stretch)
                    Text("Fills 16:10 screens without black bars. Off: the original 16:9 picture.")
                        .font(.caption).foregroundStyle(.secondary)
                    Picker("Frame pacing", selection: $model.settings.pacing) {
                        ForEach(FramePacing.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text("Smooth: each frame takes its own time (even frame times). 60 Hz steps: frames snap to 16.7 / 33.3 ms, as before; can judder below 60 FPS.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Language") {
                    Picker("Text", selection: $model.settings.textLanguage) {
                        ForEach(GameLanguage.text, id: \.code) { Text($0.label).tag($0.code) }
                    }
                    Picker("Voices", selection: $model.settings.voiceLanguage) {
                        ForEach(GameLanguage.voices, id: \.name) { Text($0.label).tag($0.name) }
                    }
                    Text("This copy officially has English, Russian, French (Canada), Spanish (Latin America), Portuguese (Brazil) and Turkish; other text languages may show English.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Overlays") {
                    Picker("FPS counter", selection: $model.settings.counter) {
                        ForEach(FrameCounter.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    .help("Game: the port's FPS counter. Metal HUD: Apple's Metal Performance HUD (FPS, GPU time, frame pacing, memory). One at a time.")
                }
            }
            .formStyle(.grouped)
            Form {
                Section("Location") {
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Text("Game folder")
                            Spacer()
                            Button("Choose…", action: model.chooseGameFolder)
                        }
                        let path = model.settings.gameDir?.path ?? "Not chosen"
                        Text(path).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
                            .lineLimit(1).truncationMode(.middle).help(path)
                        if let problem = model.gameProblem {
                            Text(problem).font(.caption).foregroundStyle(.red)
                        }
                    }
                    LabeledContent("Saves", value: "port/user in the repository")
                }
                Section("Testing for artifacts") {
                    Text("Play with the settings you want to check. If something looks wrong, note the place and the settings (General and Advanced), and the time: each session's log is in Logs (~/Library/Logs/ISS), named by its start time.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
        }
    }

    private var advanced: some View {
        HStack(alignment: .top, spacing: 0) {
            Form {
                Section("Quality") {
                    Picker("Preset", selection: $model.settings.preset) {
                        ForEach(PerformancePreset.allCases) { Text($0.label).tag(PerformancePreset?.some($0)) }
                        if model.settings.preset == nil { Text("Custom").tag(PerformancePreset?.none) }
                    }
                    .pickerStyle(.segmented)
                    Text("Sets the four choices below together. Slower Mac (for example M1 or M2 with 16 GB): lower detail, 30 FPS, 67% render scale. Slowest: lowest detail, 30 FPS, 50%, no smoke and sparks.")
                        .font(.caption).foregroundStyle(.secondary)
                    Picker("Detail", selection: $model.settings.quality) {
                        ForEach(QualityPreset.allCases) { Text($0.label).tag($0) }
                    }
                    Text("Lower and Lowest use the game's own lower settings (particles, high-resolution textures, pedestrians) for slower Macs. Original: as on the PS4.")
                        .font(.caption).foregroundStyle(.secondary)
                    Picker("Frame rate cap", selection: $model.settings.fpsCap) {
                        ForEach(FrameRateCap.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Picker("Render scale", selection: $model.settings.renderScale) {
                        ForEach(RenderScale.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text("30 FPS is steadier on slower Macs than an uneven 35-50. Render scale draws the game at a part of 1080p (sharp at 100%, softer below) and scales it up: more FPS where the GPU is the limit.")
                        .font(.caption).foregroundStyle(.secondary)
                    Picker("Upscaler", selection: $model.settings.upscaler) {
                        ForEach(Upscaler.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text("Temporal upscaling in place of the game's anti-aliasing: below 100% it rebuilds a sharp 1080p picture with the HUD at full size. Pays off in heavy scenes (fire, smoke) and on slower Macs; costs about 1 ms a frame.")
                        .font(.caption).foregroundStyle(.secondary)
                    Picker("Shadow detail", selection: $model.settings.shadowDetail) {
                        ForEach(ShadowDetail.allCases) { Text($0.label).tag($0) }
                    }
                    .pickerStyle(.segmented)
                    Text("Fewer sun-shadow cascades: distant shadows get coarser or disappear, and the game draws far fewer objects. Lowest roughly halves the draws in open scenes.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
            Form {
                Section("GPU particles") {
                    Picker("Smoke and sparks", selection: $model.settings.particles) {
                        ForEach(ParticleMode.allCases) { Text($0.label).tag($0) }
                    }
                    Text("Smoke, sparks, neon and rain are simulated on the GPU. Auto: shared memory (on, the default). Off: no GPU particles, fastest. All particles: exactly as on the PS4, much slower.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Section("Renderer") {
                    Toggle("Native GPU process", isOn: $model.settings.gpuProcess)
                    Text("The renderer runs as a native Apple Silicon process next to the game (no Rosetta for the GPU work). On by default; turn it off to run everything under Rosetta if you see a problem.")
                        .font(.caption).foregroundStyle(.secondary)
                }
            }
            .formStyle(.grouped)
        }
    }
}

/// The game stopped with an error: what happened, and a report to keep with the artifact notes.
private struct CrashReportView: View {
    let report: CrashReport
    @ObservedObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss
    @State private var copied = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("The game stopped").font(.title2.bold())
            Text(report.summary)
            ScrollView {
                Text(report.report)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(8)
            }
            .background(Color(nsColor: .textBackgroundColor))
            .clipShape(RoundedRectangle(cornerRadius: 6))
            HStack(spacing: 8) {
                if model.preparingCrashBundle {
                    ProgressView().controlSize(.small)
                    Text("Collecting macOS's crash reports and the log…").font(.caption).foregroundStyle(.secondary)
                } else if let bundle = model.crashBundle {
                    Image(systemName: "doc.zipper").foregroundStyle(.secondary)
                    Text("Diagnostics: \(bundle.lastPathComponent)").font(.caption).foregroundStyle(.secondary)
                        .textSelection(.enabled)
                    Button("Show in Finder") { model.showCrashBundle(bundle) }.controlSize(.small)
                }
            }
            HStack {
                Button("Show Log in Finder", action: model.showLog)
                Spacer()
                if copied { Text("Copied").foregroundStyle(.secondary) }
                Button("Copy Report") {
                    model.copyCrashReport()
                    copied = true
                }
                Button("Close") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 760, height: 560)
    }
}
