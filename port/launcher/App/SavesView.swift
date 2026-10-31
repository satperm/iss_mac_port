// SPDX-License-Identifier: GPL-2.0-or-later
// The launcher's save manager (Model/Saves.swift): back up, restore, start a new game.
import AppKit
import SwiftUI

struct SavesView: View {
    @ObservedObject var model: LauncherModel
    @Environment(\.dismiss) private var dismiss
    @State private var backups: [SaveBackup] = []
    @State private var selection: SaveBackup.ID?
    @State private var label = ""
    @State private var message: String?
    @State private var confirmNew = false
    @State private var confirmRestore = false

    private var store: SaveStore { model.saves }
    private var selected: SaveBackup? { backups.first { $0.id == selection } }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Saves").font(.title2.bold())
            Text(store.hasSaves
                 ? "Current saves: \(ByteCountFormatter.string(fromByteCount: SaveStore.size(of: store.saves), countStyle: .file)) in port/user/savedata. The game autosaves into two slots; a backup keeps a copy you can go back to."
                 : "No current saves: the game starts from the title screen (New Game).")
                .font(.callout).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if model.running {
                Text("The game is running: back up only. Quit the game to restore or start over.")
                    .font(.caption).foregroundStyle(.orange)
            }
            HStack {
                TextField("Name (optional), e.g. before the boss", text: $label)
                    .textFieldStyle(.roundedBorder)
                Button("Back Up Now") {
                    run { try store.backUp(label: label) == nil ? "Nothing to back up: no saves yet." : "Backed up." }
                    label = ""
                }
                .disabled(!store.hasSaves)
            }
            Table(backups, selection: $selection) {
                TableColumn("Backup") { b in Text(b.name).help(b.url.path) }
                TableColumn("Date") { b in Text(b.date.formatted(date: .abbreviated, time: .shortened)) }
                    .width(160)
                TableColumn("Size") { b in Text(ByteCountFormatter.string(fromByteCount: b.bytes, countStyle: .file)) }
                    .width(70)
            }
            .frame(minHeight: 220)
            if let message {
                Text(message).font(.caption).foregroundStyle(.secondary)
            }
            HStack {
                Button("Restore…") { confirmRestore = true }
                    .disabled(selected == nil || model.running)
                Button("Delete") {
                    guard let b = selected else { return }
                    run { try store.delete(b); return "Deleted \(b.name)." }
                }
                .disabled(selected == nil)
                Button("Show in Finder") {
                    if let b = selected {
                        NSWorkspace.shared.activateFileViewerSelecting([b.url])
                    } else {
                        try? FileManager.default.createDirectory(at: store.backups, withIntermediateDirectories: true)
                        NSWorkspace.shared.open(store.backups)
                    }
                }
                Spacer()
                Button("Start New Game…") { confirmNew = true }
                    .disabled(model.running || !store.hasSaves)
                Button("Close") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
            Text("A backup is also made automatically before every start (the newest \(SaveStore.autoKeep) are kept).")
                .font(.caption).foregroundStyle(.secondary)
        }
        .padding(20)
        .frame(width: 680, height: 520)
        .onAppear(perform: reload)
        .confirmationDialog("Start a new game?", isPresented: $confirmNew) {
            Button("Back Up and Start Over") {
                run { try store.startNewGame(); return "Saves backed up and cleared: the next start begins a new game." }
            }
        } message: {
            Text("The current saves are backed up first, then removed.")
        }
        .confirmationDialog("Restore \(selected?.name ?? "")?", isPresented: $confirmRestore) {
            Button("Restore") {
                guard let b = selected else { return }
                run { try store.restore(b); return "Restored \(b.name) (the saves before it were backed up)." }
            }
        } message: {
            Text("The current saves are backed up first, then replaced by this backup.")
        }
    }

    private func reload() {
        backups = store.list()
        if let selection, !backups.contains(where: { $0.id == selection }) { self.selection = nil }
    }

    private func run(_ action: () throws -> String) {
        do { message = try action() } catch { message = "Failed: \(error.localizedDescription)" }
        reload()
    }
}
