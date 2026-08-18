<div align="center">
  <img src="../docs/icon.png" alt="Headphone Safety icon" width="128" height="128">

  # Headphone Safety (Windows)

  A Windows tray app that protects your hearing during headphone use, bringing iOS's "Reduce Loud Sounds" behavior to Windows.

  ![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
  ![C++](https://img.shields.io/badge/C%2B%2B-17-orange)
  ![License](https://img.shields.io/badge/license-MIT-green)
</div>

---

> This is the Windows implementation. See the [repo root](../README.md) for the other platforms —
> macOS, Linux, and Android are all shipping too.

## Why this exists

iOS has had a built-in "Reduce Loud Sounds" feature for years: a real-time limiter that caps
audio peaks before they reach your headphones, independent of the volume slider. Windows has no
equivalent — there is nothing that continuously limits what actually reaches your ears.

Headphone Safety exists to close that gap, the same way it already does on
[macOS](../macos/README.md) and [Linux](../linux/README.md).

## Features

### Volume Cap

A lightweight safety ceiling on your headphone output volume.

- Caps the device's volume at a configurable amount of headroom below its maximum (0, 5, 10, 15,
  or 20 dB).
- If the volume is pushed above the cap, it's clamped back down automatically, checked several
  times a second.
- Applies only when wired or Bluetooth headphones are the active output device; speakers are left
  untouched.
- Requires no special permissions. **Fully working today.**

### Real-Time Limiter

The actual signal-level protection, equivalent to iOS's Reduce Loud Sounds. **Confirmed working
with real measured numbers, not just a checkbox** — see the verification note below.

- Routes your audio through [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) (a free,
  widely-used virtual audio driver), captures it via WASAPI loopback, runs it through a
  non-lookahead envelope-follower limiter (fast attack, slower release — same DSP as the
  [macOS](../macos/README.md) and [Linux](../linux/README.md) builds), and renders the limited
  signal to your real output device — the same architecture those two ports already use in
  production, adapted for WASAPI instead of CoreAudio/PipeWire.
- **Verified live against real Bluetooth headphones** (Sony WH-CH720N): with a 10dB headroom
  setting, a full-scale (0 dBFS) test tone measured at -0.1dB before the limiter and **-9.9dB
  after it**, matching the configured ceiling to within 0.1dB and holding rock-steady. Not a
  simulated or unit-test result — a live WASAPI loopback capture of the actual rendered output.
- An earlier approach (inserting a [Windows Audio Processing Object](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-audio-processing-objects)
  directly into the driver chain, avoiding any virtual device) was fully built and correctly
  registered but never actually got loaded by Windows' audio engine on the development machine,
  across two different driver stacks. That code is kept in [`apo/`](apo/) as a documented,
  parked reference — see [`docs/windows-port.md`](../docs/windows-port.md)'s "Known blocker"
  section — in case it works on different hardware, but it is not what ships or is installed by
  default today.
- No microphone/screen-recording permission prompt was observed during testing — WASAPI loopback
  capture from an unpackaged Win32 desktop app is not gated by Windows' microphone privacy toggle
  the way a UWP/MSIX app's capture would be. Unlike the macOS build (which needs Microphone +
  Screen Recording access to do the equivalent).
- Automatically detects and refuses to "protect" a Bluetooth device's low-quality Hands-Free
  (call/voice) endpoint if that happens to be the current default instead of its normal A2DP
  (music) endpoint — Windows exposes these as two separate devices for the same physical
  headphones, and can silently switch between them. The tray's context menu tells you why it's
  not running rather than silently limiting the wrong channel.

## How it works

**Volume Cap** reads the current default output device's volume via WASAPI's
`IAudioEndpointVolume` (native decibels, no scalar-to-dB translation needed) roughly every
350ms, and writes it back down if it exceeds the configured cap. Each poll tick runs on a worker
thread with a timeout, so a call that hangs against a device that's mid-disconnect can never
freeze the app.

**Real-Time Limiter** switches your default output device to VB-Cable, opens a WASAPI loopback
capture on it, runs each captured buffer through the limiter DSP, and renders the result to your
real output device — all on one dedicated thread (`LimiterEngine`), using MMCSS ("Pro Audio")
scheduling priority for glitch resistance:

```
Apps -> OS default output (switched to VB-Cable while limiting is active)
  -> LimiterEngine: WASAPI loopback capture (VB-Cable)
    -> envelope-follower limiter (same DSP as macOS/Linux)
    -> resample/remix if the real device's native format differs from VB-Cable's
    -> WASAPI render (your real device)
  -> hardware
```

If your real output device's native audio format differs from VB-Cable's (common with Bluetooth
headphones, which often run at 44100 Hz rather than the system's usual 48000 Hz), `LimiterEngine`
automatically falls back to the device's own native format and resamples/remixes on the fly —
confirmed live against real Bluetooth hardware, not just wired devices matching VB-Cable's format
by coincidence.

### Reliability guarantees

- Toggling the limiter off (or the tray detecting a problem — a device change, a Bluetooth
  profile switch, an engine failure) reverts the OS default output back to your real device
  first, verified via readback, before tearing down the capture/render pipeline — mirrors the
  same order-of-operations lesson already learned on the macOS build.
- **Force-killing the tray process temporarily leaves the OS default output on VB-Cable** until
  the next launch's startup recovery check runs (which happens unconditionally, every launch) —
  this is a real trade-off of this architecture, shared with the macOS/Linux ports' virtual-device
  approach, and different from the parked APO approach's structural independence from the tray
  process. Quitting normally (tray icon → Quit) always reverts cleanly first.
- Volume Cap's poll loop runs each tick through a timeout wrapper (2 seconds) on a dedicated
  worker thread — a call that hangs against a mid-disconnect device is abandoned rather than
  allowed to freeze the tray's UI thread and message loop.
- A `DeviceWatcher` (`IMMNotificationClient`) reacts to device state/default-device changes by
  triggering an immediate re-poll — verified live by disabling/re-enabling and by physically
  disconnecting/reconnecting a real Bluetooth headset while the tray was running, confirming both
  the callback firing and the process staying responsive throughout.
- No component of the rollback path queries the specific device a change notification reports —
  only `GetDefaultAudioEndpoint`, re-resolved fresh, which is what's actually being polled.
- The FIFO between capture and render never drops audio for a transient buffer-size mismatch (an
  earlier version of this code did, and it was audible — see `LimiterEngine.cpp`'s comments for
  the live measurement that caught it).

## Requirements

- Windows 10 or 11 (developed and tested against Windows 10 Pro 22H2, build 19045)
- [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) — free, required for the Real-Time
  Limiter (not for Volume Cap). No documented silent-install switch; it's a small GUI installer
  you run once yourself, same one-time-setup spirit as the macOS build's BlackHole install via
  Homebrew.
- To build from source: [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
  with the "Desktop development with C++" workload (includes CMake + Ninja)

## Installation

### 1. Install VB-Audio Virtual Cable

Download and run the installer from [vb-audio.com/Cable](https://vb-audio.com/Cable/) (accept
its own driver-signing prompt if shown). Only needed for the Real-Time Limiter — Volume Cap works
without it.

### 2. Build from source

```
git clone https://github.com/kbssrikar7/headphonesafety.git
cd headphonesafety
.\windows\build.ps1
```

This produces `windows\build\tray\HeadphoneSafetyTray.exe` (the shipped app).

### 3. Install

```
.\windows\packaging\install.ps1
```

Copies the built tray binary to `%LOCALAPPDATA%\HeadphoneSafety`, sets up autostart at login
(via Task Scheduler), and starts it immediately. Does **not** require elevation — the Real-Time
Limiter's architecture (VB-Cable + WASAPI, not a driver-level APO) has no
`TrustedInstaller`-owned registry keys to touch. Windows SmartScreen may still show an "unknown
publisher" warning the first time you run `install.ps1` if downloaded from the internet, since
this project is not code-signed — the same experience the macOS build's Gatekeeper warning and
its documented right-click-to-open workaround already ask you to accept.

If you'd rather try the parked APO approach instead (no virtual device, but not confirmed working
on any test hardware so far — see [`docs/windows-port.md`](../docs/windows-port.md)), its
registration scripts are still available under [`apo/register/`](apo/register/) and do require
elevation; this is not what `install.ps1` sets up by default.

### Uninstall

```
.\windows\packaging\uninstall.ps1
```

Stops the tray, removes the autostart task, and removes the installed binaries. Add
`-RemoveSettings` to also clear your saved headroom/enabled preferences. If you also registered
the parked APO manually, unregister it separately via `apo\register\unregister-apo.ps1`
(elevated).

## Usage

Headphone Safety runs entirely from a system tray icon — there is no taskbar window.

1. Connect wired or Bluetooth headphones.
2. Right-click the tray icon.
3. Turn on **Enable Volume Cap** and/or **Enable Real-Time Limiter**, and pick a headroom preset.
4. That's it — both features run continuously in the background.

To quit: tray icon → **Quit Headphone Safety**.

### Testing without a full install

Two CLI flags let you exercise the app without installing or elevating anything:

```
.\windows\build\tray\HeadphoneSafetyTray.exe --test-cap
```

Logs Volume Cap's every poll tick (device name, classification, current dB, ceiling, whether a
clamp was applied) to the console — useful for confirming headphone detection works on your
specific hardware before trusting the tray UI.

```
.\windows\build\tray\HeadphoneSafetyTray.exe --enable-limiter
.\windows\build\tray\HeadphoneSafetyTray.exe --disable-limiter
```

Persist the Real-Time Limiter's enabled setting and exit immediately, without launching the tray
— useful for scripting a test where a separate process plays audio while the tray (launched
normally afterward) picks up the setting.

```
.\windows\build\tray\HeadphoneSafetyTray.exe --print-default
```

Read-only: prints the current OS default output device and whether it's VB-Cable — safe to run
alongside a live tray instance to observe its effect without interfering.

## Project structure

```
windows/
├── CMakeLists.txt / build.ps1      Top-level build (CMake + Ninja via VS's Developer Shell)
├── shared_dsp/
│   └── Limiter.h/.cpp              Pure envelope-follower DSP math, no Win32 calls - shared
│                                    between the active tray build and the parked apo/ build
├── tray/                           HeadphoneSafetyTray.exe - the shipped app
│   ├── main.cpp                    Entry point, background thread + Win32 message loop
│   ├── TrayIcon.h/.cpp             Shell_NotifyIcon + context menu
│   ├── VolumeCap.h/.cpp            IAudioEndpointVolume poll/clamp
│   ├── LimiterEngine.h/.cpp        WASAPI loopback capture -> limiter -> render, one thread
│   ├── LimiterStatus.h/.cpp        Thread-safe live status (limiting/blocked-reason) for the menu
│   ├── DefaultDeviceSwitcher.h/.cpp  IPolicyConfig wrapper + Bluetooth voice-endpoint detection
│   ├── VBCableDetector.h/.cpp      Finds the "CABLE Input (VB-Audio Virtual Cable)" device
│   ├── PolicyConfig.h              Undocumented IPolicyConfig interface/GUID declarations
│   ├── DeviceWatcher.h/.cpp        IMMNotificationClient - device change reactions
│   ├── TimeoutRunner.h/.cpp        Runs a poll tick with a timeout, never blocks the caller
│   ├── SharedStateServer.h/.cpp    Writes a shared-memory status mapping (diagnostic use)
│   └── Settings.h/.cpp             HKCU-backed user preferences
├── apo/                            PARKED reference code - see docs/windows-port.md. Not shipped
│                                    by install.ps1 by default; kept building, not deleted.
├── tools/                          Dev-only diagnostics, not shipped (loopback capture w/
│                                    explicit device id, force_revert_default watchdog, IPC dump)
└── packaging/                      install.ps1 / uninstall.ps1
```

## Acknowledgments

- [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) — the free virtual audio driver the
  Real-Time Limiter routes through; the same "capture -> process -> render to real device"
  architecture this project's macOS build already uses with BlackHole.
- [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) and its
  [source mirror](https://github.com/mirror/equalizerapo) — the real-world precedent proving a
  solo/indie developer can ship a system-wide unsigned APO, and the actual reference for both the
  `DisableProtectedAudioDG` mechanism and the registry-ownership fix used by the parked `apo/`
  approach.
- [PotatoAPO](https://github.com/Dybios/PotatoAPO) — a minimal, real-device-attaching APO used as
  the structural reference for the parked `apo/` approach's COM object and registration scheme.
- [dechamps/APO](https://github.com/dechamps/APO) — technical documentation of the
  `MMDevices\Audio` registry key's TrustedInstaller ownership, independently confirmed and fixed
  while building the parked `apo/` approach.

## License

MIT — see [LICENSE](../LICENSE).
