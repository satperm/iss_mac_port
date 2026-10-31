// SPDX-License-Identifier: GPL-2.0-or-later
// ISS.app's main executable: the launcher window (as bloodborne_mac_port's, without the updater:
// this app never checks for or installs updates).
import AppKit
import SwiftUI

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var model: LauncherModel!
    private var window: NSWindow?
    private var direct = false
    private var quitAfterStop = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        guard let layout = Layout.detect(bundleURL: Bundle.main.bundleURL,
                                         info: Bundle.main.infoDictionary ?? [:]) else {
            let alert = NSAlert()
            alert.messageText = "This copy of ISS.app can't find the repository"
            alert.informativeText = "Run port/scripts/macos/make_app.sh in the repository again."
            alert.runModal()
            NSApp.terminate(nil)
            return
        }
        model = LauncherModel(layout: layout)
        model.onStarted = { [weak self] in
            guard let self else { return }
            if self.direct {
                self.window?.orderOut(nil)
                NSApp.setActivationPolicy(.accessory)
            } else {
                self.showWindow(activate: false)
            }
        }
        model.onExit = { [weak self] status in self?.gameEnded(status) }
        model.load()
        // ⌥ (Option) at open shows the window even when it is turned off.
        if !NSEvent.modifierFlags.contains(.option) && model.canSkipWindow {
            direct = true
            model.play()
        } else {
            showWindow()
        }
    }

    private func gameEnded(_ status: Int32) {
        if quitAfterStop {
            NSApp.reply(toApplicationShouldTerminate: true)
            return
        }
        if direct && status == 0 {
            NSApp.terminate(nil)
            return
        }
        direct = false
        showWindow()
    }

    private func showWindow(activate: Bool = true) {
        NSApp.setActivationPolicy(.regular)
        if window == nil {
            let window = NSWindow(contentRect: NSRect(origin: .zero, size: LauncherView.size),
                                  styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
            window.title = "inFAMOUS Second Son"
            window.contentView = NSHostingView(rootView: LauncherView(model: model))
            window.isReleasedWhenClosed = false
            window.center()
            self.window = window
        }
        if activate {
            window?.makeKeyAndOrderFront(nil)
            NSApp.activate()
        } else {
            window?.orderBack(nil)
        }
    }

    @objc func showAbout(_ sender: Any?) { About.show() }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        direct = false
        showWindow()
        return false
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        // Quitting the launcher stops a game it started.
        guard model != nil && model.running && model.stop() else { return .terminateNow }
        quitAfterStop = true
        return .terminateLater
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        model == nil || !model.running
    }

    func applicationWillTerminate(_ notification: Notification) {
        if model != nil && !model.running { model.save() }
    }
}

MainActor.assumeIsolated {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    let menu = NSMenu()
    let appItem = NSMenuItem()
    menu.addItem(appItem)
    let appMenu = NSMenu()
    appMenu.addItem(withTitle: "About ISS", action: #selector(AppDelegate.showAbout(_:)), keyEquivalent: "").target = delegate
    appMenu.addItem(.separator())
    appMenu.addItem(withTitle: "Hide ISS", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
    appMenu.addItem(.separator())
    appMenu.addItem(withTitle: "Quit ISS", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
    appItem.submenu = appMenu
    let editItem = NSMenuItem()
    menu.addItem(editItem)
    let editMenu = NSMenu(title: "Edit")
    editMenu.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
    editMenu.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
    editItem.submenu = editMenu
    app.mainMenu = menu
    app.setActivationPolicy(.regular)
    app.run()
}
