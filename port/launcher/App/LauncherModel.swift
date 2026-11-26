// SPDX-License-Identifier: GPL-2.0-or-later
// ISS launcher state (adapted from bloodborne_mac_port's LauncherModel, without the updater,
// the benchmark and the mod manager).
import AppKit
import Foundation

@MainActor
final class LauncherModel: ObservableObject {
    @Published var settings = LaunchSettings()
    /// macOS Energy Mode advice (Model/PowerMode.swift), refreshed when the window loads and before play.
    @Published private(set) var powerHint: String?
    @Published private(set) var gameProblem: String?
    @Published private(set) var running = false
    @Published private(set) var stopping = false
    @Published private(set) var exitMessage: String?
    @Published private(set) var crashReport: CrashReport?
    @Published var showingCrashReport = false
    @Published private(set) var crashBundle: URL?
    @Published private(set) var preparingCrashBundle = false
    @Published private(set) var sessionLog: URL?
    @Published var showingSaves = false
    @Published var showingControls = false
    /// This start lists and compiles the shaders first (run_macos.sh): minutes, once.
    @Published private(set) var preparingShaders = false
    /// A benchmark run is in progress (Benchmark.swift).
    @Published private(set) var benchmarking = false
    /// The last benchmark's report, shown in an alert.
    @Published var benchmarkReport: String?
    /// The new-issue link for the last benchmark (Share on GitHub).
    private(set) var benchmarkIssue: URL?
    private var benchmarkTimer: DispatchWorkItem?

    let layout: Layout
    /// The launcher's environment, plus the game binary inside the bundle (make_app.sh links it
    /// there) so macOS Game Mode sees the game as this app.
    var env: [String: String] {
        var env = ProcessInfo.processInfo.environment
        let probe = Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/tlg-probe")
        if env["TLG_PROBE"] == nil && FileManager.default.fileExists(atPath: probe.path) { env["TLG_PROBE"] = probe.path }
        return env
    }
    var onStarted: (() -> Void)?
    var onExit: ((Int32) -> Void)?
    private var gameProcess: Process?
    private var sessionStart: Date?

    let appVersion = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? "dev"
    var logsFolder: URL { Layout.logsFolder(home: URL(fileURLWithPath: NSHomeDirectory())) }
    var logURL: URL { sessionLog ?? logsFolder.appendingPathComponent("game.log") }

    init(layout: Layout) { self.layout = layout }

    var saves: SaveStore { SaveStore(port: layout.data, userDir: env["TLG_USER_DIR"]) }

    var canPlay: Bool { !running && settings.gameDir != nil && gameProblem == nil }
    /// The window can stay closed (the game starts at once) when everything needed is there.
    var canSkipWindow: Bool { !settings.showLauncher && canPlay }

    func load() {
        settings = LaunchSettings(contentsOf: layout.launchConf)
        checkGame()
        refreshPowerHint()
    }

    func refreshPowerHint() {
        DispatchQueue.global(qos: .utility).async {
            let hint = PowerState.current()?.hint
            DispatchQueue.main.async { self.powerHint = hint }
        }
    }

    func openBatterySettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.Battery-Settings.extension") {
            NSWorkspace.shared.open(url)
        }
    }

    func save() {
        try? settings.write(to: layout.launchConf)
    }

    func checkGame() {
        gameProblem = settings.gameDir.map(GameRunner.checkGame) ?? "Choose the game folder."
    }

    func chooseGameFolder() {
        let panel = NSOpenPanel()
        panel.message = "Choose the inFAMOUS Second Son folder (the one with eboot.bin and sce_sys)."
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.directoryURL = settings.gameDir
        guard panel.runModal() == .OK, let url = panel.url else { return }
        settings.gameDir = url
        checkGame()
        save()
    }

    var canBenchmark: Bool { canPlay && Benchmark.save(port: layout.port) != nil }

    /// The game from the benchmark save, in a user folder of its own (the player's saves are not
    /// touched; the shader cache is shared), without input, stopped after Benchmark.duration.
    func runBenchmark() {
        guard canBenchmark, prepareBenchmarkSave() else { return }
        benchmarking = true
        startBenchmarkRun(extra: [:], duration: Benchmark.duration)
    }

    /// The benchmark save copied into the benchmark's own user folder; false (with a message) on failure.
    private func prepareBenchmarkSave() -> Bool {
        guard let save = Benchmark.save(port: layout.port) else { return false }
        let fm = FileManager.default
        let user = layout.data.appendingPathComponent("benchmark-user")
        let savedata = user.appendingPathComponent("savedata")
        do {
            try fm.createDirectory(at: user, withIntermediateDirectories: true)
            try? fm.removeItem(at: savedata)
            try fm.copyItem(at: save, to: savedata)
            let cache = user.appendingPathComponent("cache")
            let shared = (env["TLG_USER_DIR"].map { URL(fileURLWithPath: $0) }
                          ?? layout.data.appendingPathComponent("user")).appendingPathComponent("cache")
            try? fm.createDirectory(at: shared, withIntermediateDirectories: true)
            if (try? fm.destinationOfSymbolicLink(atPath: cache.path)) == nil {
                try? fm.removeItem(at: cache)
                try fm.createSymbolicLink(at: cache, withDestinationURL: shared)
            }
        } catch {
            benchmarkReport = "The benchmark could not prepare its save: \(error.localizedDescription)"
            return false
        }
        return true
    }

    /// One benchmark game start (extra: environment on top of the settings), stopped after `duration`.
    /// Returns false when the game did not start.
    @discardableResult
    private func startBenchmarkRun(extra: [String: String], duration: TimeInterval) -> Bool {
        let user = layout.data.appendingPathComponent("benchmark-user")
        var environment = ["TLG_USER_DIR": user.path, "TLG_HOST_INPUT": "0", "TLG_FRAME_STATS": "1"]
        for (k, v) in extra { environment[k] = v }
        play(extra: environment)
        guard running else { benchmarking = false; detailedPhase = nil; return false }
        let timer = DispatchWorkItem { [weak self] in self?.stop() }
        benchmarkTimer = timer
        DispatchQueue.main.asyncAfter(deadline: .now() + duration, execute: timer)
        return true
    }

    // MARK: Detailed benchmark (Model/DetailedBenchmark.swift)

    /// 1: your settings, 2: half render scale; nil when no detailed run is in progress.
    @Published private(set) var detailedPhase: Int?
    private var detailedFull: DetailedBenchmark.Phase?
    private var detailedScale = 100
    private var detailedCap = 0

    /// 90 s at the player's settings, then 80 s of the same scene at half the render scale; the saved
    /// settings are not changed (the scale goes to the second game start as an environment variable).
    func runDetailedBenchmark() {
        guard canBenchmark, prepareBenchmarkSave() else { return }
        detailedFull = nil
        detailedScale = Int(settings.renderScale.rawValue) ?? 100
        detailedCap = Int(settings.fpsCap.rawValue) ?? min(120, NSScreen.main?.maximumFramesPerSecond ?? 60)
        detailedPhase = 1
        benchmarking = true
        startBenchmarkRun(extra: [:], duration: DetailedBenchmark.phase1Duration)
    }

    private func finishDetailedPhase() {
        benchmarkTimer?.cancel()
        benchmarkTimer = nil
        let text = sessionLog.flatMap { try? String(contentsOf: $0, encoding: .utf8) } ?? ""
        if detailedPhase == 1 {
            guard let full = DetailedBenchmark.phase(log: text, measured: DetailedBenchmark.phase1Measured,
                                                     renderScale: detailedScale, duration: DetailedBenchmark.phase1Duration) else {
                detailedPhase = nil
                benchmarking = false
                benchmarkReport = "The detailed benchmark did not reach the scene (too few statistics in the log). Run it again: the first run may still be compiling shaders."
                return
            }
            detailedFull = full
            detailedPhase = 2
            // Let the first game's processes exit before the second start.
            DispatchQueue.main.asyncAfter(deadline: .now() + 4) { [weak self] in
                guard let self, self.detailedPhase == 2 else { return }
                let half = DetailedBenchmark.halfScale(of: self.settings.renderScale)
                _ = half
                if !self.startBenchmarkRun(extra: ["TLG_BENCH_HALF_SCALE": "1"],
                                           duration: DetailedBenchmark.phase2Duration) {
                    self.finishDetailedReport(half: nil)
                }
            }
            return
        }
        let half = DetailedBenchmark.phase(
            log: text, measured: DetailedBenchmark.phase2Measured,
            renderScale: DetailedBenchmark.halfScale(of: settings.renderScale), duration: DetailedBenchmark.phase2Duration)
        finishDetailedReport(half: half)
    }

    private func finishDetailedReport(half: DetailedBenchmark.Phase?) {
        defer { detailedPhase = nil; benchmarking = false }
        guard let full = detailedFull else { return }
        let screen = NSScreen.main
        let display = screen.map { s -> String in
            let k = s.backingScaleFactor
            return String(format: "%.0fx%.0f at %d Hz", s.frame.width * k, s.frame.height * k, s.maximumFramesPerSecond)
        }
        let machine = DetailedBenchmark.Machine(system: SystemInfo.current(appVersion: appVersion), display: display,
                                                power: PowerState.current(),
                                                lowPowerMode: ProcessInfo.processInfo.isLowPowerModeEnabled)
        let report = DetailedBenchmark.Report(full: full, half: half, cap: detailedCap, machine: machine,
                                              settings: ["ISS for Mac \(appVersion)"] + settings.summary, date: Date())
        benchmarkIssue = report.issueURL
        let stamp = ISO8601DateFormatter().string(from: report.date).replacingOccurrences(of: ":", with: "-")
        let folder = logsFolder.appendingPathComponent("Benchmarks/detailed-\(stamp)")
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        try? report.json.write(to: folder.appendingPathComponent("report.json"), atomically: true, encoding: .utf8)
        try? report.text.write(to: folder.appendingPathComponent("summary.txt"), atomically: true, encoding: .utf8)
        benchmarkReport = report.text
    }

    private func finishBenchmark() {
        if detailedPhase != nil { finishDetailedPhase(); return }
        benchmarkTimer?.cancel()
        benchmarkTimer = nil
        benchmarking = false
        guard let url = sessionLog, let text = try? String(contentsOf: url, encoding: .utf8),
              let result = Benchmark.result(log: text) else {
            benchmarkReport = "The benchmark did not reach the scene (too few frame statistics in the log). Run it again: the first run may still be compiling shaders."
            return
        }
        let system = SystemInfo.current(appVersion: appVersion)
        let report = Benchmark.report(result, system: system,
                                      settings: ["ISS for Mac \(appVersion)"] + settings.summary)
        benchmarkIssue = Benchmark.issueURL(result, system: system, report: report)
        let folder = logsFolder.appendingPathComponent("Benchmarks")
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let stamp = ISO8601DateFormatter().string(from: Date()).replacingOccurrences(of: ":", with: "-")
        try? report.write(to: folder.appendingPathComponent("benchmark-\(stamp).txt"), atomically: true, encoding: .utf8)
        benchmarkReport = report
    }

    func shareBenchmark() {
        if let url = benchmarkIssue { NSWorkspace.shared.open(url) }
        benchmarkReport = nil
    }

    func play() { play(extra: [:]) }

    private func play(extra: [String: String]) {
        guard !running, settings.gameDir != nil else { return }
        save()
        if GameRunner.isGameRunning {
            exitMessage = "The game is already running. Only one copy can run at a time."
            onExit?(-1)
            return
        }
        if !GameRunner.hasRosetta {
            let alert = NSAlert()
            alert.messageText = "The game needs Rosetta 2"
            alert.informativeText = "The game runs its original PlayStation 4 code through Rosetta 2, Apple's translator for Intel code. Install it now? (macOS asks for an administrator password.)"
            alert.addButton(withTitle: "Install")
            alert.addButton(withTitle: "Cancel")
            guard alert.runModal() == .alertFirstButtonReturn else { onExit?(-1); return }
            guard GameRunner.installRosetta() else {
                exitMessage = "Rosetta 2 was not installed. In Terminal, run: softwareupdate --install-rosetta"
                onExit?(-1)
                return
            }
        }
        // The game runs from inside this bundle (Contents/MacOS/tlg-probe), so it reads this app's
        // defaults: Apple's per-app switch for the Metal Performance HUD, besides MTL_HUD_ENABLED.
        UserDefaults.standard.set(settings.metalHUD, forKey: "MetalForceHudEnabled")
        if extra.isEmpty { saves.autoBackUp() }
        var environment = env
        for (key, value) in extra { environment[key] = value }
        let plan = layout.launchPlan(settings: settings, env: environment)
        do {
            let log = try SessionLogWriter(folder: logsFolder, appVersion: appVersion)
            log.note("ISS for Mac \(appVersion) on \(SystemInfo.current(appVersion: appVersion).line); "
                     + "\(settings.summary.joined(separator: ", ")); game folder \(settings.gameDir?.path ?? "?")")
            gameProcess = try GameRunner.start(plan, log: log) { [weak self] status in self?.finished(status) }
            sessionLog = log.text
            sessionStart = log.start
            stopping = false
            running = true
            let user = env["TLG_USER_DIR"].map { URL(fileURLWithPath: $0) } ?? layout.data.appendingPathComponent("user")
            preparingShaders = env["TLG_PRECOMPILE"] != "0" && !FileManager.default.fileExists(
                atPath: user.appendingPathComponent("cache/shader-jobs.bin").path)
            exitMessage = nil
            onStarted?()
        } catch {
            exitMessage = "The game could not start: \(error.localizedDescription)"
            onExit?(-1)
        }
    }

    @discardableResult
    func stop() -> Bool {
        guard running, !stopping, let process = gameProcess, process.isRunning else { return false }
        stopping = true
        process.terminate()
        return true
    }

    private func finished(_ status: Int32) {
        if benchmarking {
            gameProcess = nil
            running = false
            preparingShaders = false
            stopping = false
            finishBenchmark()
            if detailedPhase == nil { onExit?(status) }
            return
        }
        let wasStopped = stopping
        gameProcess = nil
        running = false
        preparingShaders = false
        stopping = false
        if !wasStopped, status != 0, retryStartupCrash() {
            return
        }
        startupRetries = 0
        if status == 0 || wasStopped {
            exitMessage = nil
            crashReport = nil
            showingCrashReport = false
        } else {
            let log = (try? String(contentsOf: logURL, encoding: .utf8)) ?? ""
            let (logName, system, summary, ended) = (logURL.lastPathComponent, SystemInfo.current(appVersion: appVersion),
                                                     settings.summary, Date())
            let started = sessionStart
            let makeReport: @Sendable ([NativeCrash]) -> CrashReport = { natives in
                CrashReport(status: status, log: log, logName: logName, system: system, settings: summary,
                            home: NSHomeDirectory(), started: started, ended: ended, natives: natives)
            }
            let report = makeReport([])
            crashReport = report
            crashBundle = nil
            exitMessage = report.summary
            showingCrashReport = true
            prepareCrashBundle(report: report, status: status, makeReport: makeReport)
        }
        onExit?(wasStopped ? 0 : status)
    }

    /// MoltenVK sometimes crashes in its own Metal callbacks (a freed drawable) right after the
    /// splash screen, about 1 start in 6-8; nothing on our side avoids it without costing FPS.
    /// Such a crash within two minutes of the start is retried, twice at most in a row.
    private var startupRetries = 0
    private func retryStartupCrash() -> Bool {
        guard startupRetries < 2, let start = sessionStart, Date().timeIntervalSince(start) < 120,
              let log = try? String(contentsOf: logURL, encoding: .utf8) else { return false }
        // "Fault (signal 11)" / "Host fault (signal 11)" and the frames under it.
        let fault = log.components(separatedBy: "\n").drop { !$0.contains("ault (signal") }.prefix(16)
            .joined(separator: "\n")
        let moltenVK = ["didScheduleWithStartTime", "MTLDispatchListAppendBlock", "presentCAMetalDrawable",
                        "MVKCmdDispatch", "MTLDispatchListApply"]
        guard moltenVK.contains(where: fault.contains) else { return false }
        startupRetries += 1
        NSLog("ISS: start-up crash in MoltenVK's callbacks; starting again (%d/2)", startupRetries)
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in self?.play() }
        return true
    }

    /// In the background: macOS's crash reports of the session (into the report), then the zip.
    private func prepareCrashBundle(report: CrashReport, status: Int32,
                                    makeReport: @escaping @Sendable ([NativeCrash]) -> CrashReport) {
        preparingCrashBundle = true
        let start = sessionStart ?? Date().addingTimeInterval(-3600)
        let looksLikeCrash = (status > 128 && status < 160) || report.cause.map {
            $0.localizedCaseInsensitiveContains("signal") || $0.localizedCaseInsensitiveContains("crash")
        } == true
        let name = "crash-" + (sessionLog?.deletingPathExtension().lastPathComponent ?? SessionLog.name(start: start))
            .replacingOccurrences(of: "game-", with: "")
        let (logs, sessionLog, settingsFile, conf) = (logsFolder, sessionLog, layout.settingsFile, layout.launchConf)
        Task.detached {
            let native = CrashBundle.nativeCrashes(since: start, expectOne: looksLikeCrash)
            let full = native.crashes.isEmpty ? report : makeReport(native.crashes)
            let zip = try? CrashBundle.write(report: full, name: name, logs: logs, sessionLog: sessionLog,
                                             ipsFiles: native.files, settings: settingsFile, launchConf: conf)
            await MainActor.run {
                guard self.crashReport?.report == report.report else { return }
                self.crashReport = full
                self.crashBundle = zip
                self.preparingCrashBundle = false
            }
        }
    }

    func showLog() {
        GameRunner.run(["/usr/bin/open", "-R", logURL.path])
    }

    func openLogsFolder() {
        try? FileManager.default.createDirectory(at: logsFolder, withIntermediateDirectories: true)
        NSWorkspace.shared.open(logsFolder)
    }

    func copyCrashReport() {
        guard let crashReport else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(crashReport.report, forType: .string)
    }

    func showCrashBundle(_ url: URL) {
        NSWorkspace.shared.activateFileViewerSelecting([url])
    }
}
