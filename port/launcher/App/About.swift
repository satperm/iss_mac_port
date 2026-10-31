// SPDX-License-Identifier: GPL-2.0-or-later
import AppKit

/// The standard About panel: name, version from Info.plist and a line of credits.
@MainActor
enum About {
    static func show() {
        let info = Bundle.main.infoDictionary ?? [:]
        let short = info["CFBundleShortVersionString"] as? String ?? "dev"
        let build = info["CFBundleVersion"] as? String ?? short
        let center = NSMutableParagraphStyle()
        center.alignment = .center
        let credits = NSAttributedString(
            string: "macOS port of the inFAMOUS Second Son PC port: the original PS4 game code runs through Rosetta 2, the graphics through MoltenVK on Metal. Built on iss_pc, bbport and the shadPS4 video core.",
            attributes: [.font: NSFont.systemFont(ofSize: NSFont.smallSystemFontSize),
                         .foregroundColor: NSColor.labelColor, .paragraphStyle: center])
        NSApp.orderFrontStandardAboutPanel(options: [
            .applicationName: "ISS",
            .applicationVersion: short,
            .version: build == short ? "" : build,
            .credits: credits,
        ])
        NSApp.activate()
    }
}
