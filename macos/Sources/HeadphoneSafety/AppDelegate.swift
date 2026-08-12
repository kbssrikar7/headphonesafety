import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate {
    // NSApplication.delegate is a weak reference. Holding the instance here,
    // as static storage, keeps it alive for the process lifetime - a plain
    // `let` in main.swift can get released early under release-mode ARC
    // optimization right after `app.delegate = delegate` is assigned, before
    // applicationDidFinishLaunching ever fires.
    static let shared = AppDelegate()

    private var statusItem: NSStatusItem!
    private let monitor = AudioMonitor()

    private var currentDeviceName = "—"
    private var currentKind: OutputKind = .builtInSpeaker
    private var currentLevel: AudioMonitor.LevelInfo?
    private var isLimiterActive = false

    private static let headroomPresets: [(label: String, db: Int)] = [
        ("Max (0 dB)", 0),
        ("Loud (\u{2212}5 dB)", 5),
        ("Moderate (\u{2212}10 dB)", 10),
        ("Quiet (\u{2212}15 dB)", 15),
        ("Very Quiet (\u{2212}20 dB)", 20)
    ]

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        if let button = statusItem.button {
            // Plain text glyph instead of an SF Symbol NSImage: symbol images
            // can silently fail to composite in a bare (non-Xcode-bundled)
            // executable, leaving the status item present but blank. Text
            // rendering has no such dependency.
            button.title = "🎧"
            button.font = NSFont.menuBarFont(ofSize: 14)
        }

        monitor.onStateChange = { [weak self] name, kind, level, limiterActive in
            DispatchQueue.main.async {
                self?.currentDeviceName = name
                self?.currentKind = kind
                self?.currentLevel = level
                self?.isLimiterActive = limiterActive
                self?.rebuildMenu()
                let line = "[\(Date())] device=\(name) kind=\(kind) limiterActive=\(limiterActive) level=\(String(describing: level))\n"
                if let data = line.data(using: .utf8) {
                    if let h = FileHandle(forWritingAtPath: "/tmp/hs-verify2.log") {
                        h.seekToEndOfFile(); h.write(data); h.closeFile()
                    } else {
                        try? data.write(to: URL(fileURLWithPath: "/tmp/hs-verify2.log"))
                    }
                }
            }
        }
        monitor.start()
        rebuildMenu()
    }

    func applicationWillTerminate(_ notification: Notification) {
        monitor.stop()
    }

    private func rebuildMenu() {
        let menu = NSMenu()

        let statusTitle: String
        switch currentKind {
        case .builtInHeadphones: statusTitle = "Wired headphones connected"
        case .bluetooth: statusTitle = "Bluetooth headphones connected"
        case .builtInSpeaker: statusTitle = "Built-in speakers (cap inactive)"
        case .usbOrOther: statusTitle = "Other output device (cap inactive)"
        }
        let statusLine = NSMenuItem(title: statusTitle, action: nil, keyEquivalent: "")
        statusLine.isEnabled = false
        menu.addItem(statusLine)

        let deviceItem = NSMenuItem(title: "Device: \(currentDeviceName)", action: nil, keyEquivalent: "")
        deviceItem.isEnabled = false
        menu.addItem(deviceItem)

        let levelItem = NSMenuItem(title: "Current level: \(levelDescription())", action: nil, keyEquivalent: "")
        levelItem.isEnabled = false
        menu.addItem(levelItem)

        menu.addItem(.separator())

        let capToggle = NSMenuItem(
            title: "Enable Volume Cap",
            action: #selector(toggleCap),
            keyEquivalent: ""
        )
        capToggle.target = self
        capToggle.state = Settings.shared.capEnabled ? .on : .off
        menu.addItem(capToggle)

        let capSubmenu = NSMenu()
        for preset in Self.headroomPresets {
            let item = NSMenuItem(title: preset.label, action: #selector(setCapHeadroom(_:)), keyEquivalent: "")
            item.target = self
            item.tag = preset.db
            item.state = (Settings.shared.capHeadroomDb == preset.db) ? .on : .off
            capSubmenu.addItem(item)
        }
        let capMenuItem = NSMenuItem(
            title: "Volume Cap: \u{2212}\(Settings.shared.capHeadroomDb) dB below max",
            action: nil,
            keyEquivalent: ""
        )
        menu.setSubmenu(capSubmenu, for: capMenuItem)
        menu.addItem(capMenuItem)

        menu.addItem(.separator())

        let blackHoleInstalled = BlackHoleDetector.isInstalled
        let limiterToggle = NSMenuItem(
            title: "Real-Time Limiter" + (blackHoleInstalled ? "" : " (needs BlackHole)"),
            action: #selector(toggleLimiter),
            keyEquivalent: ""
        )
        limiterToggle.target = self
        limiterToggle.state = Settings.shared.limiterEnabled ? .on : .off
        limiterToggle.isEnabled = blackHoleInstalled
        menu.addItem(limiterToggle)

        let limiterStatus: String
        if !blackHoleInstalled {
            limiterStatus = "Install: brew install blackhole-2ch"
        } else if isLimiterActive {
            limiterStatus = "Status: active (true peak limiting)"
        } else if Settings.shared.limiterEnabled {
            limiterStatus = "Status: enabled, not currently active"
        } else {
            limiterStatus = "Status: off (using volume cap only)"
        }
        let limiterStatusItem = NSMenuItem(title: limiterStatus, action: nil, keyEquivalent: "")
        limiterStatusItem.isEnabled = false
        menu.addItem(limiterStatusItem)

        menu.addItem(.separator())
        let quitItem = NSMenuItem(title: "Quit Headphone Safety", action: #selector(quit), keyEquivalent: "q")
        quitItem.target = self
        menu.addItem(quitItem)

        self.statusItem.menu = menu
    }

    private func levelDescription() -> String {
        if isLimiterActive {
            return "limited in real time (peak-safe)"
        }

        guard let level = currentLevel else { return "—" }

        guard let currentDb = level.currentDb, let minDb = level.minDb, let maxDb = level.maxDb else {
            if let percent = level.percent {
                return "\(percent)% (device doesn't report dB)"
            }
            return "unavailable"
        }

        var text = String(format: "%.1f dB (range %.0f…%.0f dB)", currentDb, minDb, maxDb)
        if let capDb = level.capDb {
            text += String(format: ", cap %.1f dB", capDb)
        }
        return text
    }

    @objc private func toggleCap() {
        Settings.shared.capEnabled.toggle()
        rebuildMenu()
    }

    @objc private func setCapHeadroom(_ sender: NSMenuItem) {
        Settings.shared.capHeadroomDb = sender.tag
        rebuildMenu()
    }

    @objc private func toggleLimiter() {
        Settings.shared.limiterEnabled.toggle()
        rebuildMenu()
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }
}
