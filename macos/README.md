<div align="center">
  <img src="../docs/icon.png" alt="Headphone Safety icon" width="128" height="128">

  # Headphone Safety (macOS)

  A macOS menu bar app that protects your hearing during headphone use, bringing iOS's "Reduce Loud Sounds" behavior to the Mac.

  ![Platform](https://img.shields.io/badge/platform-macOS%2012%2B-blue)
  ![Swift](https://img.shields.io/badge/Swift-5-orange)
  ![License](https://img.shields.io/badge/license-MIT-green)
</div>

---

> This is the macOS implementation. See the [repo root](../README.md) for the other platforms
> (Linux is also implemented; Windows and Android are architecture guides only, not yet built).

## Why this exists

iOS has had a built-in "Reduce Loud Sounds" feature for years: a real-time limiter that caps audio peaks before they reach your headphones, independent of the volume slider. macOS has no equivalent. Its only headphone-related safety feature is a one-time loud-volume warning dialog — there is nothing that continuously limits what actually reaches your ears.

In practice, this creates a real difference in how the same headphones feel on the two devices: extended listening on an iPhone at a given volume is comfortable, while the same headphones at a similar volume on a Mac can start to cause ear pain and fatigue within a much shorter session. iOS is actively limiting the signal; macOS is not.

Headphone Safety exists to close that gap — bringing genuine, real-time audio-level protection to macOS, not just a volume ceiling.

## Features

### Volume Cap

A lightweight safety ceiling on your headphone output volume.

- Caps the device's volume at a configurable amount of headroom below its maximum (0, 5, 10, 15, or 20 dB).
- If the volume is pushed above the cap — via the keyboard volume keys, the menu bar slider, or any other app — it's clamped back down automatically, checked several times a second.
- Applies only when wired or Bluetooth headphones are the active output device; built-in speakers are left untouched.
- Requires no special permissions.

### Real-Time Limiter

The actual signal-level protection, equivalent to iOS's Reduce Loud Sounds.

- Routes system audio through a true peak-limiting Audio Unit before it reaches your headphones, so a loud transient inside content that's already playing at an "allowed" volume is caught and capped — not just the volume level, the waveform itself.
- Uses [BlackHole](https://github.com/ExistentialAudio/BlackHole), a free, open-source virtual audio loopback driver, to capture system audio, and Apple's built-in `kAudioUnitSubType_PeakLimiter` Audio Unit to enforce a hard ceiling on the output signal.
- Requires BlackHole to be installed separately (see [Requirements](#requirements)) — it is not bundled with this app.
- Requires macOS's Microphone and Screen & System Audio Recording permissions. This is a limitation of macOS's permission model, not a design choice: any real-time audio *capture* API, including reading from a virtual loopback device that carries no microphone input at all, is gated behind the same "microphone" permission as an actual physical mic. No microphone hardware is ever opened, and no audio leaves your machine — the permission prompt is unavoidable, but what it's protecting isn't actually happening.

## How it works

**Volume Cap** reads the current output device's volume via CoreAudio's native decibel properties (or a scalar-to-decibel translation, or a fallback percentage clamp, depending on what the device exposes) roughly every 400ms, and writes it back down if it exceeds the configured cap.

**Real-Time Limiter** is a manually wired chain of three raw CoreAudio Audio Units:

```
BlackHole (capture)  →  Apple PeakLimiter (in-process effect)  →  Real output device
```

When enabled with headphones connected, system default output is switched to BlackHole. A capture Audio Unit reads from BlackHole into a ring buffer; the limiter Audio Unit pulls from that buffer and applies true peak limiting with a configurable headroom (reusing the same dB presets as Volume Cap); an output Audio Unit bound directly to the real headphone device pulls the limited signal and plays it. All of this happens with tens of milliseconds of latency, inaudible for typical playback.

### Reliability and rollback guarantees

Since this reroutes system-wide audio, correctness on the way *back* out matters as much as correctness going in:

- Turning the Real-Time Limiter off reverts output to the real device on the next poll cycle (~400ms), no restart required.
- A watchdog inside the limiter detects engine failures and reverts automatically.
- Disconnecting headphones while the limiter is active is detected and reverted to a safe fallback device within about a second, with retry logic to handle the brief timing window macOS leaves right after a device disconnect where a routing change can silently fail to apply.
- If the app is killed uncleanly (crash, force-quit) while routed through BlackHole, a startup check reverts stray routing back to a real device the next time it launches.
- No component in the rollback path performs a per-device CoreAudio property query while attempting to recover — device queries against hardware that's mid-disconnect have been observed to block indefinitely, so recovery paths rely only on cached state and system-level calls that don't touch the disconnecting device directly.

## Requirements

- macOS 12 or later
- Apple Silicon or Intel — [release builds](https://github.com/kbssrikar7/headphonesafety/releases) are universal binaries, and building from source produces one too
- [Swift](https://www.swift.org/) toolchain (for building from source)
- [BlackHole 2ch](https://github.com/ExistentialAudio/BlackHole) — only required for the Real-Time Limiter feature; Volume Cap works without it:

  ```
  brew install blackhole-2ch
  ```

## Installation

### Option 1: Homebrew (recommended)

```
brew tap kbssrikar7/headphonesafety
brew install --cask headphonesafety
```

Uses the [homebrew-headphonesafety](https://github.com/kbssrikar7/homebrew-headphonesafety) tap.
To update later: `brew update && brew upgrade --cask headphonesafety`.

### Option 2: Download a release

1. Download the latest `.zip` from [Releases](https://github.com/kbssrikar7/headphonesafety/releases/latest) and unzip it.
2. Move `Headphone Safety.app` to `/Applications`.
3. **First launch**: releases are ad-hoc signed, not notarized by Apple, so Gatekeeper will initially block the app. Right-click it → **Open** → confirm **Open** in the dialog (only needed once). Alternatively: `xattr -cr "/Applications/Headphone Safety.app"` before launching.

### Option 3: Build from source

```
git clone https://github.com/kbssrikar7/headphonesafety.git
cd headphonesafety
./macos/build.sh
cp -R "macos/.build/release/Headphone Safety.app" /Applications/
open "/Applications/Headphone Safety.app"
```

### Launch at login (optional)

To have Headphone Safety start automatically at login:

```
cat > ~/Library/LaunchAgents/com.local.headphonesafety.launcher.plist <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.local.headphonesafety.launcher</string>
    <key>ProgramArguments</key>
    <array>
        <string>/usr/bin/open</string>
        <string>-a</string>
        <string>/Applications/Headphone Safety.app</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
</dict>
</plist>
EOF
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.local.headphonesafety.launcher.plist
```

To remove it later:

```
launchctl bootout gui/$(id -u)/com.local.headphonesafety.launcher
rm ~/Library/LaunchAgents/com.local.headphonesafety.launcher.plist
```

## Usage

Headphone Safety runs entirely from a menu bar icon (🎧) — there is no Dock icon or main window.

1. Connect wired or Bluetooth headphones.
2. Click the menu bar icon.
3. Turn on **Enable Volume Cap** and/or **Real-Time Limiter**, and pick a headroom preset.
4. That's it — both features run continuously in the background, reacting automatically to headphone connects, disconnects, and volume changes.

To quit: menu bar icon → **Quit Headphone Safety**.

## Project structure

```
Sources/HeadphoneSafety/
├── main.swift              Entry point
├── AppDelegate.swift        Menu bar UI
├── AudioMonitor.swift       Polling loop, state machine, dB-clamp logic, rollback safety
├── LimiterEngine.swift      Raw Audio Unit pipeline for the real-time limiter
├── CoreAudioUtils.swift     Low-level CoreAudio device/volume helpers
├── BlackHoleDetector.swift  Detects whether BlackHole is installed
├── RingBuffer.swift         Thread-safe buffer bridging capture and playback
└── Settings.swift           UserDefaults-backed user preferences
```

## Acknowledgments

- [BlackHole](https://github.com/ExistentialAudio/BlackHole) by Existential Audio — the virtual loopback driver the Real-Time Limiter depends on. Not bundled with this project; install separately via Homebrew.
- Apple's built-in `PeakLimiter` Audio Unit (`kAudioUnitSubType_PeakLimiter`), which does the actual signal limiting.

## License

MIT — see [LICENSE](../LICENSE).
</content>
