# Headphone Safety for Windows — Implementation Guide

## Context: what this is and why

This is a port of a macOS menu bar app ("Headphone Safety") that brings iOS's "Reduce Loud
Sounds" behavior to a desktop OS. The motivation: extended headphone listening on a laptop can
cause ear pain/fatigue much faster than the same headphones on a phone, because phones actively
limit audio peaks in real time and desktop OSes generally don't. The macOS version has two
independent features:

1. **Volume Cap** — a fast, simple ceiling on the output device's volume level. Cheap to build,
   weaker protection (it clamps the *volume knob*, not the actual waveform).
2. **Real-Time Limiter** — true peak limiting of the actual audio signal, so a loud transient in
   content that's already playing at an "allowed" volume gets caught and capped, not just the
   level. This is the one that actually matches what iOS does.

This document is a from-scratch build guide for the Windows equivalent, written after building
the macOS version end-to-end and researching the Windows-specific APIs needed. It assumes the
reader (a future Claude session, or a developer) has **no memory of the macOS build** — so it
explains the *why* behind each design decision, not just the *what*.

**Read the "Hard-won lessons" section before writing any code that touches device switching or
rollback.** Every one of those lessons cost real debugging time on macOS and the underlying
failure modes (timing windows, misleading success signals, blocking calls on a hot path) are
generic enough that they are very likely to recur on Windows in some form.

---

## Architecture overview

Same two-feature split as macOS, but the *how* differs a lot per feature.

### Feature 1: Volume Cap

Windows' **WASAPI** (Windows Audio Session API) exposes an `IAudioEndpointVolume` interface per
audio endpoint (output device), with **native decibel support** — better than macOS in this one
respect, where many devices only expose a linear scalar and dB has to be inferred via a
translation call. On Windows:

- `IAudioEndpointVolume::GetMasterVolumeLevel` / `SetMasterVolumeLevel` — get/set in **decibels
  directly**, no scalar-to-dB translation needed.
- `IAudioEndpointVolume::GetVolumeRange` — returns the device's actual dB range (min, max, and
  the increment step), analogous to macOS's `kAudioDevicePropertyVolumeRangeDecibels`.
- Obtain the endpoint via `IMMDeviceEnumerator::GetDefaultAudioEndpoint(eRender, eConsole)`, then
  `IMMDevice::Activate(__uuidof(IAudioEndpointVolume), ...)`.

This is the easy part. Build it first, verify it live, exactly like the macOS build did (dB cap
was built and stabilized *before* any of the real-time limiter work started).

**Detecting "is this a headphone-like device"** (so the cap only applies to headphones, not
speakers): read `PKEY_AudioEndpoint_FormFactor` via `IPropertyStore` on the `IMMDevice`. Values of
interest: `Headphones`, and for Bluetooth, check `PKEY_AudioEndpoint_JackSubType` or simply the
device's driver/interface description string for Bluetooth-related substrings — Windows doesn't
have as clean a single "transport type" enum as macOS's `kAudioDevicePropertyTransportType`, so
this may need a couple of heuristics combined (form factor + checking whether the device is a
Bluetooth Hands-Free/A2DP endpoint via its container ID or interface friendly name).

Poll loop: same approach as macOS, a timer every ~300-400ms that reads current volume, compares
to the cap, writes it back down if it's over. No permission needed for any of this.

### Feature 2: Real-Time Limiter

This is where Windows and macOS genuinely diverge, and where Windows has a real structural
advantage: **it is possible to ship a real-time audio limiter on Windows with *zero* runtime
permission prompt**, something confirmed to be architecturally impossible on macOS (see the
"macOS answer" section below for why, since that context is useful for explaining to the user why
this port is worth doing).

There are two possible approaches. **Recommend attempting Approach A first** — it's the "proper"
solution and the one with a proven real-world precedent. Fall back to Approach B only if Approach
A proves too heavy to get working.

#### Approach A (recommended): Windows Audio Processing Object (APO)

A Windows Audio Processing Object is Microsoft's official, documented mechanism for inserting
custom DSP directly into an audio endpoint's own driver-level signal chain — the same mechanism
Dolby/DTS/Realtek use for their built-in effects. Critically, because it runs *inside* the
device's own audio pipeline (not as a separate app capturing a stream), **it does not touch any
of the audio-capture permission surface at all.**

**Real-world proof this is achievable by an indie/solo developer**: [Equalizer APO]
(https://sourceforge.net/projects/equalizerapo/) is a free, open-source, community-built Windows
APO with millions of users, doing exactly this class of system-wide real-time audio processing.
It is not signed via WHQL and was not built by a hardware vendor. Study how it works — both its
source (available on SourceForge) and its installer behavior are the best reference for this
build.

**How Equalizer APO ships without WHQL certification or an EV code-signing certificate**: its
installer sets one registry value:

```
HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio\DisableProtectedAudioDG = 1  (DWORD)
```

This disables Windows' APO signature check, allowing an unsigned APO to load. This requires admin
rights during install (comparable to `sudo` for a macOS system-level component) and Windows
SmartScreen shows a one-time "unknown publisher" warning during install — but **once installed
and running, there is no ongoing permission prompt, no privacy-settings toggle, no indicator of
any kind.** This is the single most important finding from the research phase: it's the one
architecture, across macOS/Windows/Linux, that gets real signal-level protection with a
completely clean running experience.

**Implementation steps:**

1. Read Microsoft's own APO documentation in order:
   - [Windows Audio Processing Objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-audio-processing-objects) (concept overview)
   - [Audio Processing Object Architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-processing-object-architecture)
   - [Implementing Audio Processing Objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects)
   - [Windows 11 APIs for Audio Processing Objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-11-apis-for-audio-processing-objects) — check whether Windows 11 introduced a simpler/newer registration path than the classic INF-based one; this could meaningfully reduce the install complexity.
2. Pull the official sample APO from Microsoft's [`Windows-driver-samples`](https://github.com/microsoft/Windows-driver-samples) GitHub repo (there are audio APO samples under the `audio/` directory, e.g. a swap/delay APO) — start from a working sample rather than the headers alone.
3. An APO is implemented as an in-process COM object, packaged as a DLL, implementing
   (at minimum) `IAudioProcessingObject` and `IAudioProcessingObjectConfiguration`. There are two
   kinds: **LFX** (local effect, applied before mixing/per-stream) and **GFX** (global effect,
   applied after mixing, on the final mixed signal). **Use GFX** — the limiter needs to see the
   final mixed output, the same thing BlackHole captures on macOS, not any one app's individual
   stream.
4. Registering the APO against a real output device normally requires modifying that device's
   driver INF (`AudioProcessingObjects` registry section under the device's driver key) to insert
   your CLSID into the device's effects chain. Study exactly how Equalizer APO's installer does
   this — it manipulates the registry directly under
   `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\<device-guid>\FxProperties`
   rather than requiring a real driver package reinstall. This is the actual mechanism to
   replicate.
5. Get a trivial pass-through APO working and *verified audible* before writing any limiter DSP —
   this mirrors exactly how the macOS build was debugged (get capture→output working silently, as
   a no-op pass-through, before adding the PeakLimiter stage; several of the hardest macOS bugs
   were in the pass-through plumbing, not the DSP).
6. Implement the limiter DSP (see "Limiter DSP algorithm" below) inside the APO's `APOProcess`
   callback. **This callback runs on a real-time audio thread inside the audio engine process —
   it must never block, allocate, take a lock that could be contended, or touch paged memory.**
   Microsoft's docs are explicit about this (`nonblocking`, `nonpageable` requirements) — treat
   this as strictly as the macOS `AURenderCallback` real-time constraints were treated.

#### Approach B (fallback): WASAPI loopback capture + virtual cable

If Approach A proves too heavy, this mirrors the macOS architecture almost exactly and is
guaranteed to work, at the cost of the permission prompt:

1. Install a virtual audio cable driver (e.g. [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) — the closest Windows equivalent to BlackHole, though closed-source/freeware rather than open source; note this licensing difference if it matters for the project).
2. Set the virtual cable as the default output device via the `IPolicyConfig` undocumented COM
   interface (GUID `f8679f50-850a-41cf-9c72-430f290290c8`, implementation class
   `870af99c-171d-4f9e-af0d-e63df40c2bc9`) — widely used by community tools (SoundVolumeView,
   `AudioDeviceCmdlets` PowerShell module, various C# "audio switcher" projects on GitHub) for
   many years, stable since Windows 7, but **undocumented and could change** in a future Windows
   release. Reference implementation: search GitHub for `IPolicyConfig.h` (e.g.
   `tartakynov/audioswitch`).
3. Capture from the virtual cable via **WASAPI loopback capture**
   (`AUDCLNT_STREAMFLAGS_LOOPBACK` on `IAudioClient::Initialize`, targeting the cable's render
   endpoint) — see Microsoft's [Loopback Recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording) docs.
4. Apply the limiter DSP to the captured buffer.
5. Render the limited signal to the *real* output device via a second, separate `IAudioClient` in
   normal render mode.
6. **Permission**: this path is gated by Settings → Privacy → Microphone → "Allow desktop apps to
   access your microphone." A consent prompt can appear the first time. This is a lighter-weight
   gate than macOS's per-app indicator (more of a system-wide toggle, usually already on), but
   it's not permission-free — flag this clearly to the user if this fallback is what ends up
   shipping.

### Limiter DSP algorithm

Neither Windows nor Linux ships a drop-in equivalent to Apple's `kAudioUnitSubType_PeakLimiter`
that's trivially reusable from arbitrary C++ code the way it was on macOS (an in-process system
Audio Unit). Plan to implement a basic peak limiter directly — this is a well-understood, compact
algorithm:

```
For each sample:
  1. Compute the instantaneous absolute level of the sample.
  2. Run it through an envelope follower with a fast attack and slower release:
       if level > envelope: envelope += (level - envelope) * attackCoeff
       else:                envelope += (level - envelope) * releaseCoeff
     (attackCoeff/releaseCoeff derived from desired attack/release times in ms and the sample rate,
     e.g. coeff = 1 - exp(-1 / (timeMs * 0.001 * sampleRate)))
  3. If envelope exceeds the ceiling (the headroom-derived threshold, matching the app's existing
     dB-preset concept), compute the needed gain reduction: gain = ceiling / envelope (capped at 1.0
     when envelope is below ceiling, i.e. no reduction).
  4. Multiply the (delayed, if using lookahead) sample by gain.
```

A simple non-lookahead version (attack ~1-5ms, release ~50-100ms) is enough for this use case and
is much simpler to get right in a real-time-safe way than a lookahead limiter with a delay buffer.
Reuse the same dB-headroom-preset concept from the macOS app's `Settings` (0/5/10/15/20 dB) as the
ceiling parameter, for consistency across the platforms if this is ever presented as one product.

---

## Hard-won lessons from the macOS build (read before writing rollback logic)

The macOS build went through multiple live-tested rounds where a seemingly-safe revert mechanism
turned out not to be safe in practice. These are the actual root causes found — treat all of them
as *generically likely to recur* on Windows, not macOS-specific quirks:

1. **A "successful" API call is not proof the change took effect.** On macOS,
   `AudioObjectSetPropertyData` for the default output device silently returns `noErr` even when
   targeting a device ID that no longer exists — it just no-ops. Any Windows equivalent (setting
   the default device via `IPolicyConfig`, or any other property-set call) should be **verified
   by reading the value back**, not trusted from its return code alone.

2. **There is a real timing window right after a device disconnects where operations can
   transiently fail/no-op, even targeting a device that's still valid.** On macOS, a revert
   attempt to a guaranteed-present device (built-in speakers) failed silently in the split-second
   right after a Bluetooth disconnect event, then succeeded a moment later when retried. Any
   revert/rollback logic needs to **retry until it actually lands**, not attempt once and give up
   — implemented on macOS as a continuous self-healing check in the idle poll loop, not a
   one-shot "try, and if it fails, try the fallback" attempt.

3. **Per-device property queries against a device mid-disconnect can block the calling thread
   indefinitely.** This is the most dangerous failure mode found: a plain, read-only property
   query (not even a write) against a device that's in the middle of a Bluetooth teardown froze
   the app's entire main thread, which also froze the poll loop that was supposed to detect and
   fix the problem — a self-inflicted deadlock of the safety mechanism itself. **Design rule
   coming out of this**: cache whatever device IDs/handles are needed for rollback *once, at
   session start, while everything is healthy* — never re-enumerate or re-query live device state
   from inside a recovery/rollback path. On Windows, watch for the equivalent: any WASAPI or
   `IMMDevice`/`IPropertyStore` call against an endpoint that's actively being removed. Use
   `IMMNotificationClient::OnDeviceStateChanged` / `OnDefaultDeviceChanged` to detect removal
   *without* querying the removed device itself for anything.

4. **A device-list-change listener should react unconditionally, not re-verify by querying.** The
   first (buggy) macOS fix tried to have the disconnect-listener itself check "is my device still
   in the list" before reacting — but that check was itself the kind of query that could hang.
   The working fix: any device-list-change notification while the limiter is running is treated
   as "assume something's wrong, revert" unconditionally. A false-positive revert (e.g. from an
   unrelated USB device being plugged in) is cheap — the limiter just restarts on the next tick if
   the real device is still there. A hang is not recoverable. Bias toward the cheap failure mode.

5. **Test the actual failure scenario live, not just the happy path.** Every fix above was found
   by actually disconnecting real Bluetooth headphones while the limiter was running and watching
   what happened — not by reasoning about the API contracts alone (the API docs/behavior did not
   predict several of these failure modes). Budget real time for this kind of testing on Windows
   too: toggle on/off, force-kill the process while the limiter is active, and physically
   disconnect Bluetooth headphones mid-playback, each followed by confirming audio is actually
   restored (not just that no error was logged).

6. **Independent, out-of-process safety net during development.** While iterating and testing
   live, it was valuable to have a *separate* watchdog process (not part of the app under test)
   that would force-revert output after a timeout regardless of what the app itself was doing —
   this caught cases where the app's own recovery logic was still broken, without leaving the
   developer stuck with no sound for an extended period during testing. Recommend the same
   approach here: a small standalone script/process, launched before each live test, that
   force-sets the default output device back to something safe after e.g. 15-20 seconds no matter
   what.

---

## Recommended build order

Mirrors how the macOS version was actually built and debugged:

1. **Volume Cap only.** Get this fully working and stable first — no permission concerns, low
   risk, and it's the fallback behavior for everything else.
2. **APO skeleton, pure pass-through, no DSP.** Get audio flowing through an installed, registered
   APO completely unmodified, and *confirm it's audible* before adding any processing. (On macOS,
   several of the hardest bugs were in getting audio to flow through the pipeline at all — the
   actual limiting logic was comparatively simple once the plumbing worked.)
3. **Add the limiter DSP** to the pass-through APO, verify audibly that it's a true peak limiter
   (test with a deliberately loud/clipping test tone, confirm the output stays below the ceiling
   even when the input is well above it — don't just confirm "it sounds a bit quieter", confirm
   the actual peak values via logging).
4. **Rollback and safety testing**, following the lessons above: toggle-off, crash recovery,
   device-disconnect recovery — each verified live, each with a documented test showing it
   actually works, not just "should work" per the API docs.
5. **UI**: a system tray icon (Windows Forms `NotifyIcon`, WPF equivalent, or a lighter framework)
   with a menu mirroring the macOS app's structure — device status, cap toggle + headroom presets,
   limiter toggle + status line.

---

## Distribution notes

- Ad-hoc/self-signed is fine for personal use, same as the macOS build. For any wider
  distribution, note that DisableProtectedAudioDG plus an unsigned APO will show Windows
  SmartScreen's "unknown publisher" warning on install — document this for users the same way the
  macOS README documents the Gatekeeper right-click-to-open step.
- If using Approach B (virtual cable), document the separate driver install step clearly, the same
  way the macOS README documents installing BlackHole via Homebrew as a prerequisite.

## Testing checklist (same bar as the macOS version)

- [ ] Volume Cap clamps correctly on a headphone-classified device, leaves speakers untouched.
- [ ] Real-Time Limiter is audible and does not silently do nothing.
- [ ] Confirmed via logged peak values that a deliberately loud/clipping input signal is actually
      capped at the output, not just passed through at a slightly lower level.
- [ ] Toggling the limiter off reverts output cleanly and promptly.
- [ ] Force-killing the process while the limiter is active does not leave the system silently
      stuck — either it stays fine (APO approach, since it's not "this app's process" that's doing
      the routing) or it recovers automatically (Approach B, on next launch).
- [ ] Physically disconnecting headphones while the limiter is active reverts to a safe output
      device automatically, without the app hanging.
- [ ] No component of the rollback path performs a live query against a device that might be
      mid-disconnect.

## Key references

- [Loopback Recording (Win32)](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
- [Windows Audio Processing Objects overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-audio-processing-objects)
- [Implementing Audio Processing Objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/implementing-audio-processing-objects)
- [Audio Processing Object Architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/audio-processing-object-architecture)
- [Windows 11 APIs for Audio Processing Objects](https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/windows-11-apis-for-audio-processing-objects)
- [Microsoft's Windows-driver-samples (GitHub)](https://github.com/microsoft/Windows-driver-samples) — real sample APO source
- [Equalizer APO (SourceForge)](https://sourceforge.net/projects/equalizerapo/) — the real-world precedent proving this is achievable solo; study both its installer behavior and, if available, its source
- [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) — fallback-approach virtual device
- `IPolicyConfig` reference implementation: search GitHub for `tartakynov/audioswitch`

## Why this doesn't exist on macOS (context, in case it comes up)

For context if this is ever compared side-by-side with the macOS version: macOS's equivalent
driver mechanism (`AudioServerPlugIn`) is explicitly, structurally forbidden from calling back
into the CoreAudio client API itself — confirmed directly in Apple's own
`AudioServerPlugIn.h` header ("may not make any calls to the client HAL API in the
CoreAudio.framework"). That means a macOS driver can *present* a virtual device but can never
itself forward processed audio to a *different* physical device — something else in userspace
always has to do that bridging, and that bridging is exactly what triggers macOS's Microphone/
Screen Recording permission. This was confirmed via multiple real-world precedents (Background
Music, Rogue Amoeba's Loopback/SoundSource) all hitting the same wall despite each owning their
own driver. Windows' APO model has no equivalent restriction — an APO can process audio as a
first-class part of a device's own pipeline, which is why Approach A above is possible at all.
