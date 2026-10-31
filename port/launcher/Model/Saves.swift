// SPDX-License-Identifier: GPL-2.0-or-later
// Save backups: copies of port/user/savedata in port/out/save-backups/<name>/savedata (the game only
// autosaves into two slots, so going back or starting over means swapping that folder).
import Foundation

struct SaveBackup: Identifiable, Equatable {
    let url: URL
    let date: Date
    let bytes: Int64
    var id: URL { url }
    var name: String { url.lastPathComponent }
    /// Made by the launcher before a start (pruned to the newest SaveStore.autoKeep).
    var isAuto: Bool { name.hasSuffix(" auto") }
}

struct SaveStore {
    let saves: URL
    let backups: URL
    static let autoKeep = 10

    /// userDir: TLG_USER_DIR when set (test runs), else port/user.
    init(port: URL, userDir: String? = nil) {
        saves = (userDir.map { URL(fileURLWithPath: $0) } ?? port.appendingPathComponent("user"))
            .appendingPathComponent("savedata")
        backups = port.appendingPathComponent("out/save-backups")
    }

    private var files: FileManager { .default }

    var hasSaves: Bool { Self.size(of: saves) > 0 }

    func list() -> [SaveBackup] {
        let names = (try? files.contentsOfDirectory(at: backups, includingPropertiesForKeys: [.contentModificationDateKey])) ?? []
        return names.compactMap { url in
            let data = url.appendingPathComponent("savedata")
            guard files.fileExists(atPath: data.path) else { return nil }
            let date = (try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
            return SaveBackup(url: url, date: date, bytes: Self.size(of: data))
        }.sorted { $0.date > $1.date }
    }

    /// A copy of the current saves; nil when there are none.
    @discardableResult
    func backUp(label: String) throws -> SaveBackup? {
        guard hasSaves else { return nil }
        try files.createDirectory(at: backups, withIntermediateDirectories: true)
        let stamp = Self.stamp.string(from: Date())
        let clean = label.replacingOccurrences(of: "/", with: "-").trimmingCharacters(in: .whitespaces)
        var folder = backups.appendingPathComponent(clean.isEmpty ? stamp : "\(stamp) \(clean)")
        var n = 2
        while files.fileExists(atPath: folder.path) {
            folder = backups.appendingPathComponent("\(stamp) \(clean) \(n)")
            n += 1
        }
        try files.createDirectory(at: folder, withIntermediateDirectories: true)
        try files.copyItem(at: saves, to: folder.appendingPathComponent("savedata"))
        let data = folder.appendingPathComponent("savedata")
        return SaveBackup(url: folder, date: Date(), bytes: Self.size(of: data))
    }

    /// Before a start: a backup marked "auto", keeping the newest autoKeep of them.
    func autoBackUp() {
        guard (try? backUp(label: "auto")) != nil else { return }
        for old in list().filter(\.isAuto).dropFirst(Self.autoKeep) {
            try? files.removeItem(at: old.url)
        }
    }

    /// The current saves are backed up first, then replaced by the backup's.
    func restore(_ backup: SaveBackup) throws {
        try backUp(label: "before restore")
        if files.fileExists(atPath: saves.path) { try files.removeItem(at: saves) }
        try files.createDirectory(at: saves.deletingLastPathComponent(), withIntermediateDirectories: true)
        try files.copyItem(at: backup.url.appendingPathComponent("savedata"), to: saves)
    }

    /// The current saves are backed up, then removed: the game starts from its title screen.
    func startNewGame() throws {
        try backUp(label: "before new game")
        if files.fileExists(atPath: saves.path) { try files.removeItem(at: saves) }
    }

    func delete(_ backup: SaveBackup) throws {
        try files.removeItem(at: backup.url)
    }

    static func size(of folder: URL) -> Int64 {
        guard let items = FileManager.default.enumerator(at: folder, includingPropertiesForKeys: [.fileSizeKey, .isRegularFileKey]) else { return 0 }
        var total: Int64 = 0
        for case let url as URL in items {
            let values = try? url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey])
            if values?.isRegularFile == true { total += Int64(values?.fileSize ?? 0) }
        }
        return total
    }

    private static let stamp: DateFormatter = {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd HH-mm-ss"
        return f
    }()
}
