// SPDX-License-Identifier: GPL-2.0-or-later
// ISS launcher: where things are. ISS.app lives in the repository (scripts/macos/make_app.sh);
// the game runs through port/run_macos.sh.
import Foundation

/// How to start the game: a command, its environment and folder.
struct LaunchPlan {
    var executable: URL
    var arguments: [String]
    var environment: [String: String]
    var directory: URL
}

struct Layout {
    /// The repository (iss_mac_port).
    let repo: URL
    /// A release app (package_app.sh): repo is the bundle's Resources, which nothing writes to;
    /// settings, saves and the game image go here (Application Support/ISS).
    var dataDir: URL? = nil
    var port: URL { repo.appendingPathComponent("port") }
    /// Where settings, saves and the prepared game image live.
    var data: URL { dataDir ?? port }
    /// TLG_LAUNCH_CONF: another file for test runs (as bloodborne_mac_port's BB_LAUNCH_CONF).
    var launchConf: URL {
        ProcessInfo.processInfo.environment["TLG_LAUNCH_CONF"].map { URL(fileURLWithPath: $0) }
            ?? data.appendingPathComponent("launch.conf")
    }
    var settingsFile: URL { data.appendingPathComponent("tlg.ini") }
    static func logsFolder(home: URL) -> URL { home.appendingPathComponent("Library/Logs/ISS") }

    /// From Info.plist's ISSRepo (make_app.sh), else the folder that holds the app.
    static func detect(bundleURL: URL, info: [String: Any]) -> Layout? {
        let resources = bundleURL.appendingPathComponent("Contents/Resources")
        if FileManager.default.fileExists(atPath: resources.appendingPathComponent("port/run_macos.sh").path) {
            let support = FileManager.default.homeDirectoryForCurrentUser
                .appendingPathComponent("Library/Application Support/ISS")
            let data = ProcessInfo.processInfo.environment["TLG_DATA_DIR"].map { URL(fileURLWithPath: $0) } ?? support
            try? FileManager.default.createDirectory(at: data, withIntermediateDirectories: true)
            return Layout(repo: resources, dataDir: data)
        }
        let repo = (info["ISSRepo"] as? String).flatMap { $0.isEmpty ? nil : URL(fileURLWithPath: $0) }
            ?? bundleURL.deletingLastPathComponent()
        let layout = Layout(repo: repo)
        return FileManager.default.fileExists(atPath: layout.port.appendingPathComponent("run_macos.sh").path) ? layout : nil
    }

    /// The game through run_macos.sh with the launcher's settings in the environment (a variable
    /// already set in the launcher's own environment wins for diagnostics).
    func launchPlan(settings: LaunchSettings, env: [String: String]) -> LaunchPlan {
        var environment = env
        for (key, value) in settings.environment where environment[key] == nil {
            environment[key] = value
        }
        environment["TLG_PREBUILT"] = environment["TLG_PREBUILT"] ?? "1"
        if let dataDir {
            environment["TLG_DATA_DIR"] = dataDir.path
            environment["PYTHON"] = repo.appendingPathComponent("python/bin/python3").path
            environment["PYTHONDONTWRITEBYTECODE"] = "1"
        }
        return LaunchPlan(executable: URL(fileURLWithPath: "/bin/bash"),
                          arguments: [port.appendingPathComponent("run_macos.sh").path],
                          environment: environment, directory: port)
    }
}
