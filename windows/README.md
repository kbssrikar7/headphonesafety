<div align="center">
  <img src="../docs/icon.png" alt="Headphone Safety icon" width="128" height="128">

  # Headphone Safety (Windows)

  A Windows tray app that protects your hearing during headphone use, bringing iOS's "Reduce Loud Sounds" behavior to Windows.

  ![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-blue)
  ![C++](https://img.shields.io/badge/C%2B%2B-17-orange)
  ![License](https://img.shields.io/badge/license-MIT-green)
</div>

---

> This is the Windows implementation. See the [repo root](../README.md) for the other platforms
> (macOS and Linux are shipping; Android is an architecture guide only).

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

The actual signal-level protection, equivalent to iOS's Reduce Loud Sounds.

- Inserts a true peak-limiting [Windows Audio Processing Object (APO)](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-audio-processing-objects)
  directly into your audio driver's own signal chain — the same mechanism Dolby/DTS/Realtek use
  for their own built-in effects. Because it runs inside the device's own pipeline rather than as
  a separate capture process, it never touches any microphone/audio-capture permission surface —
  **no permission prompt of any kind**, unlike the macOS build (which needs Microphone + Screen
  Recording access) or a WASAPI-loopback-based approach.
- **Known limitation, please read before relying on this**: whether the APO actually gets loaded
  by Windows' audio engine depends on your specific audio driver. It's confirmed correctly built
  and registered (verified via direct COM instantiation and registry readback surviving a full
  reboot), but on the primary development machine's Conexant ISST driver, the audio engine never
  loads it — confirmed via live ETW tracing showing the APO is never even attempted for
  discovery, after trying every fix short of kernel debugging (service restart, process kill,
  full reboot, PnP re-enumeration). This may be driver-specific rather than a fundamental Windows
  limitation. See [`docs/windows-port.md`](../docs/windows-port.md)'s "Known blocker" section for
  the full investigation. **Try it on your own hardware — it may just work.**

## How it works

**Volume Cap** reads the current default output device's volume via WASAPI's
`IAudioEndpointVolume` (native decibels, no scalar-to-dB translation needed) roughly every
350ms, and writes it back down if it exceeds the configured cap. Each poll tick runs on a worker
thread with a timeout, so a call that hangs against a device that's mid-disconnect can never
freeze the app.

**Real-Time Limiter** is a Windows Audio Processing Object — an in-process COM DLL registered
directly into a specific output device's driver-level effects chain (the `SFX`/`MFX` "stream
effect"/"mode effect" slots, paired with the endpoint's supported processing modes — see
[`register-apo.ps1`](apo/register/register-apo.ps1) for the exact mechanism). When enabled, its
`APOProcess` callback runs a non-lookahead envelope-follower limiter (fast attack, slower
release) directly on the audio buffer, on the real-time audio thread inside `audiodg.exe`, with
no rerouting of your default output device at all — unlike the macOS/Linux ports, which switch
the OS default output to a virtual device. A tray-to-APO shared-memory mapping lets the tray's
enabled/headroom settings reach the APO instance without either process needing to know about
the other beyond that one named mapping.

```
Real output device's own driver pipeline
  -> HeadphoneSafetyApo.dll (SFX/MFX slot, in-process)
    -> envelope-follower limiter, reading enabled/headroom from shared memory
  -> hardware
```

### Reliability guarantees

- Toggling the limiter off flips a shared-memory flag read by the real-time thread — `APOProcess`
  becomes a plain `memcpy` pass-through immediately, no stream restart, no rerouting to revert.
- Force-killing the tray process does not affect the limiter at all — it runs inside `audiodg.exe`,
  entirely independent of the tray's own process lifetime, a structural safety advantage over the
  macOS/Linux ports (whose limiters do depend on their controlling process staying alive to revert
  a rerouted default device).
- Volume Cap's poll loop runs each tick through a timeout wrapper (2 seconds) on a dedicated
  worker thread — a call that hangs against a mid-disconnect device is abandoned rather than
  allowed to freeze the tray's UI thread and message loop.
- A `DeviceWatcher` (`IMMNotificationClient`) reacts to device state/default-device changes by
  triggering an immediate re-poll — verified live by disabling and re-enabling the test machine's
  audio device while the tray was running, confirming both the callback firing and the process
  staying responsive throughout.
- No component of the rollback path queries the specific device a change notification reports —
  only `GetDefaultAudioEndpoint`, re-resolved fresh, which is what's actually being polled.

## Requirements

- Windows 10 or 11 (developed and tested against Windows 10 Pro 22H2, build 19045)
- To build from source: [Visual Studio 2022 Build Tools](https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2022)
  with the "Desktop development with C++" workload (includes CMake + Ninja), and the
  [Windows Driver Kit](https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk)
  matching your Windows SDK version

## Installation

### Build from source

```
git clone https://github.com/kbssrikar7/headphonesafety.git
cd headphonesafety
.\windows\build.ps1
```

This produces `windows\build\tray\HeadphoneSafetyTray.exe` and
`windows\build\apo\HeadphoneSafetyApo.dll`.

### Install

From an **elevated** PowerShell (installing the Real-Time Limiter APO requires admin rights —
see the note below on exactly what that involves):

```
.\windows\packaging\install.ps1
```

This copies the built binaries to `%LOCALAPPDATA%\HeadphoneSafety`, registers the Real-Time
Limiter against every headphone-like output device currently connected, sets up the tray to
start automatically at login (via Task Scheduler), and starts it immediately.

**What elevation actually does here, so there are no surprises**: registering the APO requires
setting `DisableProtectedAudioDG` (a Windows setting that permits loading an unsigned APO — the
same mechanism [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) uses, and the
reason this project's APO doesn't need a paid code-signing certificate) and taking ownership of
one specific, normally `TrustedInstaller`-owned registry key per device (the standard
`SeTakeOwnershipPrivilege` technique — the registry equivalent of `takeown.exe` + `icacls` for
the filesystem, not any kind of security bypass). Windows SmartScreen may show an "unknown
publisher" warning the first time you run `install.ps1` if downloaded from the internet, since
this project is not code-signed — this is the same experience the macOS build's Gatekeeper
warning and its documented right-click-to-open workaround already ask you to accept.

A newly-paired Bluetooth headset after installation needs one more elevated registration pass —
run `install.ps1` again (or `apo\register\register-apo.ps1 -EndpointGuid <guid>` directly, after
finding the GUID via `-ListDevices`) once it's paired. This is a real, accepted limitation of the
classic (non-UWP) APO registration model, not an oversight.

### Uninstall

From an elevated PowerShell:

```
.\windows\packaging\uninstall.ps1
```

Stops the tray, removes the autostart task, unregisters the APO from every device it was
attached to, and removes the installed binaries. Add `-RemoveSettings` to also clear your saved
headroom/enabled preferences.

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
normally afterward) keeps the shared-memory setting current.

## Project structure

```
windows/
├── CMakeLists.txt / build.ps1      Top-level build (CMake + Ninja via VS's Developer Shell)
├── shared/include/
│   └── hps_shared_state.h          Tray <-> APO shared-memory IPC contract
├── apo/                            HeadphoneSafetyApo.dll (real-time constraints apply)
│   ├── HeadphoneSafetyApo.h/.cpp   IAudioProcessingObject(Configuration) COM object
│   ├── ApoProcess.cpp              APOProcess real-time callback ONLY
│   ├── Limiter.h/.cpp              Pure envelope-follower DSP math, no Win32 calls
│   ├── SharedStateClient.h/.cpp    Reads the shared-memory mapping (opened off the RT thread)
│   ├── ClassFactory.h/.cpp, ApoDll.cpp/.def   Standard COM registration plumbing
│   └── register/                   register-apo.ps1 / unregister-apo.ps1
├── tray/                           HeadphoneSafetyTray.exe (no real-time constraints)
│   ├── main.cpp                    Entry point, background thread + Win32 message loop
│   ├── TrayIcon.h/.cpp             Shell_NotifyIcon + context menu
│   ├── VolumeCap.h/.cpp            IAudioEndpointVolume poll/clamp
│   ├── DeviceWatcher.h/.cpp        IMMNotificationClient - device change reactions
│   ├── TimeoutRunner.h/.cpp        Runs a poll tick with a timeout, never blocks the caller
│   ├── SharedStateServer.h/.cpp    Writes the shared-memory mapping
│   └── Settings.h/.cpp             HKCU-backed user preferences
├── tools/                          Dev-only diagnostics, not shipped (loopback capture, IPC dump)
└── packaging/                      install.ps1 / uninstall.ps1
```

## Acknowledgments

- [Equalizer APO](https://sourceforge.net/projects/equalizerapo/) and its
  [source mirror](https://github.com/mirror/equalizerapo) — the real-world precedent proving a
  solo/indie developer can ship a system-wide unsigned APO, and the actual reference for both the
  `DisableProtectedAudioDG` mechanism and the registry-ownership fix this project needed.
- [PotatoAPO](https://github.com/Dybios/PotatoAPO) — a minimal, real-device-attaching APO used as
  the structural reference for `HeadphoneSafetyApo`'s COM object and registration scheme.
- [dechamps/APO](https://github.com/dechamps/APO) — technical documentation of the
  `MMDevices\Audio` registry key's TrustedInstaller ownership, which this project independently
  confirmed and fixed.

## License

MIT — see [LICENSE](../LICENSE).
