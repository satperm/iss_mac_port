// SPDX-License-Identifier: GPL-2.0-or-later
// ISS launcher state (adapted from bloodborne_mac_port's LauncherModel, without the updater,
// the benchmark and the mod manager).
import AppKit
import Foundation

@MainActor
final class LauncherModel: ObservableObject {
    @Published var settings = LaunchSettings()
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
    /// This start lists and compiles the shaders first (run_macos.sh): minutes, once.
    @Published private(set) var preparingShaders = false

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

    func play() {
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
        saves.autoBackUp()
        let plan = layout.launchPlan(settings: settings, env: env)
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
