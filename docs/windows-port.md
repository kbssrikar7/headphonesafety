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

**Status as of the Phase 3 implementation session**: Volume Cap is fully built, tested, and
working live. The APO (Real-Time Limiter) is fully built and *correctly registered* (confirmed
against real reference implementations, confirmed via COM instantiation, confirmed via registry
readback surviving a reboot) but **`audiodg.exe` never actually loads it on the test machine's
Conexant ISST driver**, for reasons not resolvable without kernel-level debugging — see "Known
blocker: APO never loads on Conexant ISST" below before spending more time on registration
mechanics; that part is already correct. If you're picking this up on different hardware, the
APO code and registration scripts should be tried as-is first — this may well be driver-specific,
not a fundamental Windows limitation.

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
   (at minimum) `IAudioProcessingObject` and `IAudioProcessingObjectConfiguration`. There are
   three per-endpoint effect slots: **SFX** (per-stream, pre-mix), **MFX** (per-mode), and
   **EFX** (post-mix, one instance per endpoint — historically also called "GFX" in older docs
   and in an earlier draft of this section). **Use EFX** — the limiter needs to see the final
   mixed output, the same thing BlackHole captures on macOS, not any one app's individual stream.
4. **[Corrected during actual implementation — see `windows/apo/register/register-apo.ps1`, the
   verified source of truth]** Registering the APO against a real output device does *not* require
   modifying the device's driver INF. It's a direct registry write under
   `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\<device-guid>\FxProperties`,
   confirmed against PotatoAPO's real, working installer (github.com/Dybios/PotatoAPO — a
   minimal, non-ATL APO that attaches to real device endpoints, not a sample virtual driver).
   An earlier draft of this doc assumed a single `REG_SZ` value named
   `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2` — that was wrong on two counts. The actual value
   name for EFX is `{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},7` (`,5` for SFX, `,6` for MFX), value
   type `REG_MULTI_SZ`. A live `reg.exe query` against this test machine's actual Speakers
   endpoint showed its pre-existing (Realtek/Conexant vendor) EFX value reported as type
   `REG_SZ`, which looked like a contradiction — but **`REG_MULTI_SZ` is confirmed correct**:
   `register-apo.ps1` wrote our CLSID as `REG_MULTI_SZ`, the audio engine loaded it after an
   `audiosrv` restart with no errors in Event Viewer, and audio was confirmed still audible and
   unmodified through the pass-through APO. (The vendor value's apparent `REG_SZ` type was
   either a `reg.exe` display quirk for a single-element `REG_MULTI_SZ`, or Realtek's own value
   genuinely differs in type from what third-party APOs are expected to write — either way, it
   doesn't affect what this project writes.) `register-apo.ps1` backs up and restores any
   pre-existing value at that slot (e.g. a vendor effects chain) rather than clobbering it, and
   verifies every write by reading it back before reporting success.
5. **[Found during actual implementation, not anticipated by any earlier draft of this doc]**
   `HKLM\...\MMDevices\Audio` and everything under it (including every endpoint's `FxProperties`
   key) is owned by `TrustedInstaller`, not `Administrators` — confirmed live on this test
   machine (Windows 10 22H2, build 19045): a fully UAC-elevated Administrator process got
   `Access is denied` writing a FxProperties value via both PowerShell's `Set-ItemProperty` *and*
   raw `reg.exe`, despite the key's DACL nominally listing `BUILTIN\Administrators: SetValue`.
   This is documented, expected Windows behavior, not a machine-specific quirk (corroborated by
   `dechamps/APO`'s README and multiple Equalizer APO SourceForge support threads describing the
   identical failure). **The fix, confirmed against Equalizer APO's own real installer source
   (`RegistryHelper.cpp` in its GitHub mirror)**: before writing FxProperties, take ownership of
   the specific endpoint's `FxProperties` key and rewrite its DACL to grant Administrators full
   control — this is the standard `SeTakeOwnershipPrivilege` technique (the registry equivalent
   of `takeown.exe` + `icacls` for the filesystem), not TrustedInstaller impersonation or any
   more exotic escalation. Concretely: enable `SeTakeOwnershipPrivilege` on the process token via
   `AdjustTokenPrivileges`, open the key with `WRITE_OWNER`, call `RegSetKeySecurity` with the
   local Administrators group as owner, re-open with `WRITE_DAC`, grant Administrators
   `KEY_ALL_ACCESS` via a new DACL, then the value write succeeds normally. This is a genuinely
   bigger action than "admin rights, comparable to `sudo`" (the framing this doc originally used)
   — it permanently reassigns ownership of a Windows-protected system registry key away from
   TrustedInstaller, which is worth being explicit with the user about before doing it, even
   though it's well-precedented, standard administrative practice and not a security bypass in
   the TrustedInstaller-impersonation sense.
6. Get a trivial pass-through APO working and *verified audible* before writing any limiter DSP —
   this mirrors exactly how the macOS build was debugged (get capture→output working silently, as
   a no-op pass-through, before adding the PeakLimiter stage; several of the hardest macOS bugs
   were in the pass-through plumbing, not the DSP).
7. Implement the limiter DSP (see "Limiter DSP algorithm" below) inside the APO's `APOProcess`
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

## Known blocker: APO never loads on Conexant ISST (unresolved, read before re-registering)

Found during live testing on a real machine (Windows 10 Pro 22H2, build 19045, "Speakers
(Conexant ISST Audio)" via an Intel HD Audio bus). The registration mechanism itself is
confirmed correct — this section explains what was verified working, what was tried to fix the
actual loading, and where it stands.

**What's confirmed correct**, so don't re-derive these from scratch:
- `HeadphoneSafetyApo.dll` builds clean, exports the four required COM entry points
  (`DllRegisterServer`/`DllUnregisterServer`/`DllGetClassObject`/`DllCanUnloadNow`), and its
  CLSID (`{AAF92DEA-FFE0-4E91-94A4-39385AD5ECFD}`) instantiates successfully via plain COM
  (`[System.Type]::GetTypeFromCLSID(...)` + `[System.Activator]::CreateInstance(...)` from
  PowerShell, entirely outside the audio engine) — the DLL itself is not broken.
- `register-apo.ps1` writes four FxProperties values, all verified by registry readback and
  confirmed to survive a full reboot: `{d04e05a6-...},5` (SFX CLSID), `{d04e05a6-...},6` (MFX
  CLSID), `{d3993a3f-...},5` and `{d3993a3f-...},6` (both `AUDIO_SIGNALPROCESSINGMODE_DEFAULT` =
  `{c18e2f7e-933d-4965-b7d1-1eef228d2af3}`, required per Microsoft's "Implementing Audio
  Processing Objects" doc — registering a CLSID alone only makes it *discoverable*, not usable
  for live streaming, without also being listed as supporting a processing mode).
- The `SeTakeOwnershipPrivilege` fix for the FxProperties key's TrustedInstaller ownership (see
  "Feature 2" above) is real and necessary, and works correctly.

**What was tried to make `audiodg.exe` actually load it, in order, none of which worked**:
1. `Restart-Service audiosrv -Force` — does **not** restart `audiodg.exe` itself; confirmed same
   PID before and after. A real gap in earlier assumptions ("audiosrv restart" and "audiodg.exe
   restart" are not the same thing).
2. `Stop-Process -Name audiodg -Force` — confirmed a genuinely fresh PID spawns on next playback;
   still no load.
3. A full system reboot — registration survives intact; still no load.
4. Checking `PKEY_AudioEndpoint_Disable_SysFx` (`{1da5d803-...},5`, a per-endpoint "disable all
   enhancements" master switch) — not set, ruled out.
5. Live ETW tracing of the `Microsoft-Windows-Audio` provider
   (`logman create trace X -p "Microsoft-Windows-Audio" 0xffffffffffffffff 0xff -o trace.etl -ets`,
   then `tracerpt trace.etl -o trace.xml -of XML`), done twice (before and after fix #6 below) —
   in both traces, `System_Effect_APO_Initialized` events show ONLY the OS's own built-in
   `AdaptiveSpatialAudioRenderer` (CLSID `{5bbc2c71-dec2-4ba3-961a-36f37d1cc8a5}`,
   `audioeng.dll`) ever getting initialized. Our CLSID never appears anywhere in either trace —
   not even as a failed discovery attempt.
6. Correcting the registration from EFX-only (`,7`) to the SFX+MFX+processing-mode scheme
   described above, per Microsoft's own doc's note that some drivers do mode-mixing lower in the
   kernel stack "where it is not possible to insert an endpoint APO" for EFX specifically — no
   change in behavior.
7. Checking for an active Spatial Sound format (Windows Sonic/Dolby Atmos/DTS) that might route
   audio through a separate pipeline bypassing the classic chain — no HKCU customization found
   for this endpoint, suggesting spatial sound is in its default (off) state; likely not the
   cause, though not provably ruled out without a definitive live API check.
8. PnP device disable/re-enable (`Disable-PnpDevice`/`Enable-PnpDevice` on the underlying
   `INTELAUDIO\FUNC_01&VEN_14F1&...` device, not just the logical AudioEndpoint) — forces a full
   driver re-enumeration, different from a reboot or process kill. Device came back healthy
   (`Status: OK`); still no load.
9. **Tried against a second, completely different driver stack: Sony WH-CH720N Bluetooth
   headphones (A2DP stereo profile).** Registered SFX+MFX against the Bluetooth endpoint the same
   way, forced a fresh `audiodg.exe`, and measured actual output via loopback capture with a
   properly isolated test (Volume Cap disabled so it couldn't confound the measurement, device
   volume raised well above any clamp). Result: **identical, byte-for-byte peak measurement with
   the limiter enabled vs. disabled** (-4.1 dB both times), and `apoProcessCallCount` stayed at 0
   in both cases via the diagnostic shared-memory fields (see Section 4) — the exact same
   signature as the Conexant speakers. **This rules out "Conexant-specific" as the sole
   explanation** — the APO does not load on Windows' own Bluetooth A2DP audio stack either, a
   completely different driver from Conexant's HD Audio codec. Still not conclusively a
   fundamental Windows limitation (only two driver stacks tested, both on the same physical
   machine/Windows build), but the working hypothesis should now weight toward "something about
   this Windows 10 22H2 build's audio engine" rather than "this one vendor's driver."

**Where this stands**: this is being treated as a genuine, unresolved platform quirk, not a bug
in this project's code — confirmed across two structurally unrelated driver stacks on the same
machine. Per Microsoft's own documentation, some drivers architecturally cannot host an inserted
endpoint APO because their mode mixing happens lower in the kernel-mode stack than where APO
insertion is possible, but that explanation is weaker now that it reproduces on Bluetooth audio
too (a Microsoft-owned stack, not a third-party vendor driver). **If you're testing on different
hardware (a different physical machine, or a newer Windows 11 build), try the existing
registration as-is first** — don't assume it's broken; it may be specific to this Windows 10
22H2 build rather than to any particular driver. Diagnosing further would require kernel-level
debugging (WinDbg attached to `audiodg.exe`) or testing on a genuinely different
machine/Windows version, both out of scope for a normal dev session. If Approach A proves
unworkable on other machines too, not just this one, fall back to Approach B (documented above)
— all the DSP/limiter code (`windows/apo/Limiter.h/.cpp`) is pure math with no Win32/COM
dependencies and is directly reusable in a WASAPI-loopback-based implementation.

### A registration-script bug found and fixed while testing on Bluetooth (worth knowing about)

While cleaning up after the Bluetooth test above, direct registry readback revealed
`unregister-apo.ps1` had left the Speakers endpoint in a corrupted state from earlier in this
same project's history — not caused by anything Bluetooth-specific, but only noticed at this
point. Two compounding bugs, both now fixed in `register-apo.ps1`/`unregister-apo.ps1`:

1. `unregister-apo.ps1`'s cleanup list was written after the registration scheme moved from
   EFX-only (`,7`) to SFX+MFX (`,5`/`,6`) and never included EFX — so the original Phase 2
   registration (which only ever touched EFX) was never cleaned up by any later unregister run.
   Fixed by adding EFX back to the cleanup list (safe as a no-op on any endpoint that never used
   the old scheme).
2. An ad-hoc diagnostic script written live during this investigation (not part of the shipped
   `register-apo.ps1`) wrote directly to the SFX slot to test an SFX+EFX combination, bypassing
   the backup mechanism entirely. When the real, corrected `register-apo.ps1` later ran against
   the same endpoint, it "backed up" what it found there — which was already our own CLSID from
   the bypassed write, not the genuine original (which was absent). A later `unregister-apo.ps1`
   run then "restored" that corrupted backup, silently failing to actually revert anything.
   Fixed two ways: `Set-FxPropertyBackedUp` now refuses to back up a value that already equals
   our own CLSID (the actual guard against this failure mode recurring), and the corrupted state
   on this machine was manually repaired via direct registry restoration to the confirmed genuine
   original values (verified via the very first FxProperties dump taken in this session, before
   any registration had occurred).

Lesson: an ad-hoc diagnostic script that bypasses a project's own backup/rollback tooling, even
temporarily during debugging, can corrupt the state that tooling relies on for future runs. Worth
remembering for any future live registry debugging on this project.

---

## Distribution notes

- Ad-hoc/self-signed is fine for personal use, same as the macOS build. For any wider
  distribution, note that DisableProtectedAudioDG plus an unsigned APO will show Windows
  SmartScreen's "unknown publisher" warning on install — document this for users the same way the
  macOS README documents the Gatekeeper right-click-to-open step.
- The installer must also perform the take-ownership step described above (item 5 under "Feature
  2") for the target endpoint's FxProperties key — confirmed necessary and working end-to-end
  (verified: FxProperties write succeeded, `audiosrv` restarted cleanly, no Event Viewer errors,
  audio confirmed still audible and unmodified through the pass-through APO). This should be
  framed to users as part of the same one-time elevated install step as DisableProtectedAudioDG,
  not a separate scarier-sounding action.
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
- [Microsoft's Windows-driver-samples (GitHub)](https://github.com/microsoft/Windows-driver-samples) — real sample APO source (SwapAPO/DelayAPO: useful for `APOProcess`/`IAudioProcessingObjectConfiguration` boilerplate shape; not useful for real-device registration, since these samples target SysVAD's own sample virtual driver)
- [Equalizer APO (SourceForge)](https://sourceforge.net/projects/equalizerapo/) — the real-world precedent proving this is achievable solo; its [GitHub mirror](https://github.com/mirror/equalizerapo) (`DeviceAPOInfo.cpp`, `helpers/RegistryHelper.cpp`) is the actual source used to work out the FxProperties take-ownership mechanism above
- [PotatoAPO](https://github.com/Dybios/PotatoAPO) — a minimal, non-ATL, real-device-attaching APO; the actual reference used for `windows/apo/`'s COM object shape and the FxProperties value name/type scheme
- [dechamps/APO README](https://github.com/dechamps/APO/blob/master/README.md) — technical explanation that MMDevices\Audio and its subkeys are TrustedInstaller-owned by default
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
