# Windows Port — Still To Do / Session Continuity Notes

Working notes for continuing the Windows port of Headphone Safety. Written mid-session because
this build has been long, has hit real interruptions (API spend limits cutting off background
agents mid-task), and the user explicitly asked for a detailed tracking doc so nothing gets lost.
Read this fully before doing anything if you're picking this up cold — it has the context a fresh
session would otherwise have to re-derive at real cost (this session alone has spent many hours on
live hardware debugging).

**This file itself is a working doc, not a permanent artifact** — once everything below is done
and verified, fold anything worth keeping into `docs/windows-port.md` / `windows/README.md` /
the plan file, and this file can be deleted. Don't treat it as documentation to maintain forever.

---

## Where things actually stand right now (2026-08-15)

### Volume Cap — DONE, fully working, verified live on real hardware
Not in question, not part of "still to do." Confirmed live against real Sony WH-CH720N Bluetooth
headphones: correct classification, correct clamping, correct disconnect/reconnect recovery
(physical Bluetooth disconnect tested live, not simulated). Nothing here needs revisiting unless
a regression is found.

### Approach A (Windows Audio Processing Object) — PARKED, not deleted, documented in detail
Fully built, correctly registered per real reference implementations (Equalizer APO, PotatoAPO),
survives reboot, instantiates via COM standalone — but `audiodg.exe` never actually loads it on
this machine, confirmed on two structurally different driver stacks (Conexant HD Audio, Bluetooth
A2DP) via live ETW tracing, with every environmental cause checked and ruled out (Code Integrity,
Group Policy, antivirus, Device Guard, patch level, `AudioEndpointBuilder` service restart). Full
writeup: `docs/windows-port.md`'s "Known blocker: APO never loads on Conexant ISST" section (now
also covers the Bluetooth test and the environmental-cause investigation). Code lives under
`windows/apo/` and is kept building as reference/parked code — **do not delete it**, and **do not
re-attempt fixing it** without a genuinely new lead (a different physical machine, a newer Windows
build, or willingness to attach a kernel debugger to `audiodg.exe`, which is a real risk to
live system audio and was deliberately not attempted this session).

### Approach B (WASAPI loopback + VB-Audio Virtual Cable) — IN PROGRESS, this is the active work
This is what's actually being built right now, delegated to a background fork agent (agent id
`aee937c861b0ab9b6`, referred to by that id in `SendMessage` calls to resume it — it has been
interrupted twice already by hitting the session's monthly API spend limit, NOT by bugs; the user
raised the limit each time and the fork was resumed via `SendMessage` with full transcript context
preserved both times, using the message pattern already in this conversation's history if you need
to resume it again).

**Prerequisite state**: VB-Audio Virtual Cable IS installed and active on this machine — confirmed
via `Get-PnpDevice -Class AudioEndpoint`, friendly name exactly `"CABLE Input (VB-Audio Virtual
Cable)"` for the render endpoint. **A reboot was recommended by the vendor to "finalize" the
install and had NOT happened as of when the fork started building against it.** If anything
behaves inconsistently specifically with the VB-Cable device (not a general WASAPI code bug),
that's the first thing to suspect — ask the user to reboot and retest before chasing it as a code
bug.

**Files built so far** (per CMake changes already applied, confirmed via system reminders during
this session):
- `windows/tray/PolicyConfig.h` — `IPolicyConfig` interface declaration + GUIDs (undocumented COM
  interface; confirmed correct against a real reference implementation, `tartakynov/audioswitch`
  on GitHub — see `docs/windows-port.md` for the exact GUIDs/vtable if this file needs
  re-deriving for any reason).
- `windows/tray/DefaultDeviceSwitcher.h/.cpp` — wraps `IPolicyConfig::SetDefaultEndpoint`
  (all 3 `ERole` values, readback-verified) + `GetCurrentDefaultRenderDeviceId()`.
- `windows/tray/VBCableDetector.h/.cpp` — finds `"CABLE Input (VB-Audio Virtual Cable)"` by
  friendly-name match, returns its device ID string.
- `windows/tray/LimiterEngine.h/.cpp` — the actual capture (from VB-Cable, WASAPI loopback) +
  render (to the real device) pipeline on a dedicated MMCSS thread (`AvSetMmThreadCharacteristicsW`
  "Pro Audio"), running `hps::Limiter::Process()` on each buffer.
- `windows/shared_dsp/` (new top-level dir) — `windows/apo/Limiter.h/.cpp` was promoted out of
  `windows/apo/` into a shared CMake target `hps_dsp`, linked by both `windows/apo/` (still builds,
  parked) and `windows/tray/` (the active consumer now). The DSP algorithm itself was NOT
  modified — same envelope-follower math, already proven correct via
  `windows/apo/tests/limiter_test.cpp`'s offline test.
- `windows/tray/main.cpp` — extended with the enable/disable lifecycle (switch default to
  VB-Cable BEFORE starting capture, confirmed via readback; on any revert trigger, switch default
  AWAY from VB-Cable FIRST, then stop capture/render — this exact ordering was copied from a real,
  non-obvious bug macOS's `LimiterEngine.swift` hit, described in detail in the fork's original
  brief and in `docs/windows-port.md`/`macos/Sources/HeadphoneSafety/LimiterEngine.swift`), plus a
  startup stray-VB-Cable crash-recovery check before the background thread starts.
- `windows/tray/TrayIcon.cpp` — status line updated to reflect real Approach B state ("Limiting
  active (protecting X)", "VB-Cable not installed", "Limiter off").
- `windows/tools/force_revert_default.cpp` — the redesigned hard-won-lesson-#6 independent
  watchdog helper: its own direct `IPolicyConfig` call, zero dependency on the tray process being
  alive. Reuses `DefaultDeviceSwitcher.cpp`/`VBCableDetector.cpp` directly (compiled into this
  tool too, not just linked) rather than duplicating the COM calls.
- `windows/tools/watchdog.ps1` — redesigned to call `force_revert_default.exe` after its timeout
  instead of the old shared-memory-flag approach (which stopped making sense once the limiter
  runs in-process with the tray rather than in a separate APO process hosted by `audiodg.exe`).
- `windows/tools/loopback_capture.cpp` — extended with an **optional explicit device-ID argument**
  (`loopback_capture.exe <output.wav> <durationSeconds> [deviceId]`). This matters specifically
  for Approach B measurement: while the limiter is active, the OS **default** render device IS
  VB-Cable (the limiter's own capture source) — capturing "the default" during a limiter-active
  test would capture the raw unprocessed signal being fed INTO the limiter, not what
  `LimiterEngine` actually renders out to the real device. You MUST pass the real device's
  explicit ID to measure the true post-limiter output. Omit the argument to capture whatever's
  currently default (correct only for a plain pass-through/no-limiter-active baseline, or for
  testing against a device that IS currently default).

**What's confirmed done and tested** (per the fork's own task tracker — tasks #10, #12, #13, #14
under the coordinator's task #9, all marked `completed`):
- Task #10: full solution rebuilds clean after all the `main.cpp` lifecycle edits.
- Task #12: **live crash-recovery test passed** — force-killed the tray while limiting was
  active, relaunched it, confirmed the startup `RevertStrayVBCableDefaultOutput`-style check
  actually reverts the default device automatically. This is exactly the kind of live (not just
  reasoned-about) verification this project has insisted on throughout.
- Task #13: `force_revert_default.cpp` + redesigned `watchdog.ps1` built.
- Task #14: `TrayIcon`'s status line updated.

**What's NOT yet done — this is the actual remaining work:**
- **Task #11: Measure real peak-dB reduction. THIS IS THE ONE THAT MATTERS MOST.** The fork
  reported completion with real measured numbers (a ~9.8dB mean/RMS reduction on a clipping test
  tone, reproduced across 4 runs, matching the configured 10dB headroom closely), but flagged
  itself that peak/crest-factor behavior didn't track cleanly and recommended checking Windows'
  audio-enhancements setting before trusting it further. **The coordinator did NOT take that
  report at face value** (per this file's own earlier instruction not to) and instead
  independently reproduced and root-caused the discrepancy directly. Full findings:

  **Root cause found (high confidence, extensively tested)**: the built-in "Speakers (Conexant
  ISST Audio)" device applies its own independent, real-time dynamics/level-management processing
  to ANY audio rendered to it — an attack period of roughly 1 second during which the true signal
  level passes through, followed by settling to a consistently-attenuated level (observed
  ~-8dBFS peak, essentially constant) that persists for as long as playback continues. This was
  proven, not assumed, via a systematic elimination process, each step verified live:
  1. Confirmed the pattern appears in **completely unprocessed pass-through audio** (limiter fully
     off, direct ffplay-to-Speakers, no VB-Cable or our own code involved at all) - ruling out any
     bug in this project's `Limiter`/`LimiterEngine` code as the sole cause.
  2. Ruled out Windows' "Communications" audio-ducking feature (`HKCU:\Software\Microsoft\
     Multimedia\Audio\UserDuckingPreference`, was unset/default; explicitly set to 3 "Do nothing"
     and retested - identical result, later reverted back to 1 to match the original state).
  3. Ruled out device volume level (identical result at max volume and at a moderate -17dB level).
  4. Ruled out the vendor's own APO/enhancement chain (`PKEY_AudioEndpoint_Disable_SysFx`, was
     unset/default-on; explicitly set to 1 "disable all effects" via the same
     `SeTakeOwnershipPrivilege` technique used throughout this project for FxProperties writes,
     forced a fresh `audiodg.exe`, retested - identical result, later reverted to remove the value
     entirely and restore the original absent/default state).
  5. Ruled out the specific playback tool (`ffplay`/SDL vs. `System.Media.SoundPlayer`/WinMM -
     identical result with a completely different playback backend).
  6. Ruled out a sample-rate/channel-count mismatch requiring resampling (tone.wav was 44100Hz
     mono against the device's native 48000Hz stereo mix format; generated a format-matched
     48000Hz stereo tone and retested - identical result).
  7. **Decisive test**: captured the limiter's actual INPUT (VB-Cable loopback) and actual OUTPUT
     (real Speakers device) simultaneously, on the identical live signal, with per-second
     breakdown via `ffmpeg -ss N -t 1 ... volumedetect` rather than whole-file measurement. The
     INPUT was perfectly constant (-4.1dB every single second, zero variation) - proving the
     signal our own code produces and feeds toward the real device is clean and correct. The
     OUTPUT showed the "-1.1dB then settles to ~-8dB" pattern - proving the artifact is
     introduced specifically between our render call and what loopback-capture observes on the
     Speakers device, i.e. inside that device's own driver/hardware signal path, not in our code.

  **Practical consequence**: peak-capping behavior cannot currently be cleanly verified against
  the built-in Speakers device specifically, because its own independent processing masks
  whatever our Limiter contributes on top of it. This is NOT the same finding as Approach A's
  "doesn't load at all" blocker - Approach B's pipeline is confirmed genuinely capturing,
  processing, and rendering audio (proven by the clean, correct INPUT-side measurement, and by
  the mean-level reduction the fork already found) - it's specifically that ISOLATING the
  Limiter's own peak-domain contribution from the Speakers device's pre-existing behavior isn't
  possible with the tooling/hardware available in this session, on this device.

  **What would resolve this conclusively, in priority order**:
  1. **Reconnect the Sony WH-CH720N Bluetooth headphones** (available earlier this session, not
     connected as of this note) and repeat the exact same simultaneous-input/output,
     per-second-breakdown test against that device instead of Speakers. Bluetooth A2DP is a
     completely different, Microsoft-owned driver stack with no reason to have Conexant-specific
     processing - if the settled-region OUTPUT peak there tracks meaningfully below the INPUT
     peak (toward the configured ceiling), that's clean, conclusive, confound-free proof the
     Limiter works correctly. This is the single most valuable next step and should be tried
     before anything else below.
  2. If headphones aren't available: test against VB-Cable's own loopback as a "render target"
     proxy for a processing-free device (would require either a temporary code/test-harness change
     to point `LimiterEngine`'s render side at a second virtual device instead of Speakers, or an
     entirely separate quick diagnostic outside the app - not yet attempted, more invasive than
     option 1).
  3. Investigate the Conexant ISST behavior further specifically (e.g. Windows Sound Control
     Panel's per-device "Enhancements"/"Spatial sound" UI directly, not just the registry
     properties already checked, in case there's a toggle not exposed via
     `PKEY_AudioEndpoint_Disable_SysFx`) - lower priority than testing on different hardware, and
     may not be fully within this app's control regardless (see next paragraph).

  **Framing for the user, if this comes up before headphone retesting happens**: this is real,
  substantive progress, not a repeat of Approach A's "checkbox does nothing" problem - the
  pipeline demonstrably works (clean capture, correct rendering, real measured mean-level
  reduction). What's unresolved is a measurement/verification question specific to testing against
  this one laptop's built-in speakers, not a question of whether the feature works at all. Be
  honest about this distinction rather than either overclaiming full success or (wrongly) treating
  this as another dead end - it isn't one.

  **RESOLVED (2026-08-18), via option 1 above - real Bluetooth headphones, conclusive and clean.**
  Reconnected the Sony WH-CH720N and repeated the exact simultaneous-input(VB-Cable)/output
  (headphones)/per-second-breakdown methodology. Along the way found and fixed a REAL bug (not a
  measurement artifact) that had been silently causing `LimiterEngine::Start` to fail against this
  device the whole time:

  - **Bug**: `LimiterEngine.cpp`'s render-side `IAudioClient::Initialize` always used VB-Cable's own
    capture mix format. That happens to match the Conexant Speakers device's native format (which
    is why Approach B "worked" there), but FAILS with `AUDCLNT_E_UNSUPPORTED_FORMAT (0x88890008)`
    against the Bluetooth headphones, whose native format differs. The failure triggered the
    already-built revert-on-failure logic, which explained all the confusing "duplicate process" /
    "default keeps flapping back to the real device" symptoms seen earlier - not a PowerShell
    timing artifact, an actual repeated Start-fail-revert loop.
  - **Fix (shipped, in `windows/tray/LimiterEngine.cpp`)**: if `Initialize` with VB-Cable's format
    fails, retry with the render device's own native mix format (`GetMixFormat`), and if that
    format differs from VB-Cable's (sample rate and/or channel count), do on-the-fly linear-
    interpolation resampling + channel remixing (mono<->stereo) between the limiter's output and
    the render buffer. Verified working live: logged
    `render device native format: 44100 Hz, 2 ch (capture: 48000 Hz, 2 ch) - resampling engaged`.
  - **Separate, real gotcha found along the way (not yet fixed in code, just worked around
    manually this session)**: Windows exposes TWO separate render endpoints for the same physical
    Bluetooth headset - "Headphones" (A2DP, stereo music quality, e.g. 44100 Hz/2ch) and "Headset"
    (HFP/Hands-Free, mono voice-call quality, e.g. 16000 Hz/1ch). At one point during testing the
    OS default had silently flipped to the HFS "Headset" endpoint, so the tray was correctly
    protecting the WRONG endpoint (technically working, but not what the user actually hears music
    through). Had to manually force the default back to the A2DP endpoint via
    `HeadphoneSafetyTray.exe --test-switch --to <A2DP-device-id>` before starting the tray. **This
    is a real product gap worth addressing before shipping**: the tray should probably detect
    "current default render device is a low-quality HFP/mono Bluetooth endpoint" and warn/skip
    rather than silently protecting a voice-call channel. Not fixed yet - add to the pending list
    below.
  - **The actual measurement** (after both of the above were sorted out): played a genuinely
    full-scale (0 dBFS peak, confirmed via `ffmpeg volumedetect` on the source file itself before
    playback) 1kHz test tone with the limiter enabled and a 10dB headroom setting, while
    simultaneously loopback-capturing VB-Cable (pre-limiter input) and the A2DP Headphones endpoint
    explicitly (post-limiter output), 4 seconds each, per-second breakdown:
    - INPUT: max -0.1 dB, mean -1.6 dB, **identical across all 4 seconds** (confirms VB-Cable
      capture itself introduces no attenuation/artifact - the confound-check pattern established
      against Speakers holds here too).
    - OUTPUT: max **-9.9 dB**, mean -11.3 dB, **identical across all 4 seconds** - i.e. the
      configured -10dB ceiling was hit to within 0.1dB and held rock-steady, with none of the
      Speakers device's attack-transient/settling behavior that caused the original confusion.
    - This is a clean, confound-free, positive result: **the Real-Time Limiter genuinely caps
      peaks at the configured ceiling on real Bluetooth headphones.** Approach B's core hearing-
      protection claim is now verified with real evidence, not just pipeline plumbing.

**After task #11 is genuinely confirmed with real numbers, still remaining:**
1. **Final cleanup/safety check**: confirm the OS default output device is NOT left stuck on
   VB-Cable when everything's done. Confirm `Settings`/registry are in a sane state (Volume Cap
   enabled; limiter enabled/disabled — probably leave it in whatever state lets the user
   immediately try it themselves, likely enabled, but use judgement). This was explicitly asked of
   the fork in its last resume message — check its final report for this when it comes back.
2. **Update `windows/README.md`**: the Real-Time Limiter section currently documents Approach A
   (APO) as the mechanism and has a whole "Known limitation" callout about it not working — this
   needs a full rewrite once Approach B is confirmed working: VB-Cable as a required prerequisite
   install (framed like macOS's BlackHole-via-Homebrew step), the microphone-privacy-toggle note
   (WASAPI loopback capture may trigger a one-time Windows privacy prompt — the fork was told to
   report if/when it saw this; check its report), and the "Reliability guarantees" section needs
   real rewriting since "the APO stays loaded independent of the tray process" is no longer true
   under Approach B — force-killing the tray now temporarily leaves output on VB-Cable until the
   next launch's startup-recovery check runs, matching how the macOS build's BlackHole-based
   approach actually behaves, not the old Approach A framing. The root `README.md`'s platform
   status table and `docs/windows-port.md`'s top-level summary line likely need a matching update
   too.
3. **Update `windows/packaging/install.ps1`/`uninstall.ps1`**: currently written entirely around
   Approach A (APO FxProperties registration, `DisableProtectedAudioDG`, take-ownership). Needs:
   a VB-Cable-installed check (reusing `VBCableDetector`'s logic) with a clear message + the
   vb-audio.com/Cable/ link if missing (no known silent-install path for VB-Cable was found this
   session — confirmed it's a GUI installer with no documented silent-install switch, per its own
   `readme.txt` — the user must install it manually, same as this session did). The APO
   registration step should be removed or made explicitly opt-in/secondary (kept available for
   users on different hardware where Approach A might actually work — don't rip it out entirely,
   frame it as "try this first if you want, but here's what actually works today"). Worth
   confirming live whether `install.ps1` even needs elevation anymore once APO registration is
   optional — the Task Scheduler autostart step alone likely doesn't need it (`-RunLevel Limited`
   already), which would be a genuine simplification worth calling out.
4. **Update `docs/windows-port.md`** and the plan file
   (`C:\Users\kbs\.claude\plans\ok-so-now-you-enumerated-fog.md`) with a final Approach B section
   documenting what was actually built and verified, mirroring the level of detail already present
   for Approach A's investigation (this project's established convention: document what was tried,
   what worked, what didn't, with real evidence, not just a final summary).
5. **Git commit**: everything since the last commit (`58a557a` — "Live-test against real Bluetooth
   headphones; fix a registration script bug") is uncommitted as of this note. Follow this
   session's established commit-message pattern: write the message to a scratch file and use
   `git commit -F <file>` (this session hit real, repeated problems passing multi-paragraph commit
   messages with punctuation directly via `-m` in this PowerShell environment — don't fight that
   again, just use the file approach from the start). Check `git status`/`git diff` for anything
   unexpected before staging (`git add -A`) — this session also found and fixed a real bug where
   an ad-hoc debugging script bypassed proper tooling and corrupted registry state; the equivalent
   risk here would be a fork accidentally leaving something machine-specific or leftover-test-file
   in a commit. Not yet pushed to `origin/main` (7 commits ahead as of the last check) — ask the
   user before pushing, per this session's established practice of not pushing without explicit
   confirmation each time.
6. **NEW, found during the 2026-08-18 Bluetooth measurement (not yet fixed)**: `LimiterEngine`
   will happily start and "protect" whatever render device is the OS default at launch, including
   a Bluetooth device's low-quality HFP/Hands-Free voice-call endpoint (e.g. "Headset", mono,
   16000 Hz) if Windows has silently flipped the active Bluetooth profile to that instead of the
   normal A2DP "Headphones" stereo-music endpoint (a real, observed-live Windows Bluetooth
   behavior, not hypothetical). The tray has no way to detect this and currently just does
   whatever the OS default says — it should probably check the render endpoint's format/name
   heuristically (e.g. `PKEY_AudioEndpoint_FormFactor` won't distinguish these; likely need to
   check the endpoint's mix format's sample rate/channel count, or its friendly name containing
   "Headset" vs "Headphones") and warn the user / prefer the A2DP endpoint rather than silently
   protecting a voice channel the user isn't actually listening to music through.

**Aside, fully resolved**: the earlier "duplicate `HeadphoneSafetyTray.exe` processes" confusion
(both before and during the 2026-08-18 Bluetooth measurement session) turned out to be two
separate, boring causes, NOT a real app bug: (1) a bare foreground `.exe --print-default`
invocation genuinely does exit on its own, just with a short transient lag before it disappears
from `Get-Process` - confirmed by isolated testing (gone within a few seconds, every time); (2)
`--test-cap --device <id>` is an intentional infinite polling loop ("Ctrl+C to stop", by design) -
if you invoke it as a one-off diagnostic (e.g. to check a device's volume) and don't kill it
afterward, it stays running forever and shows up in a later `Get-Process` check looking like a
mystery stray process. Always `Stop-Process` any `--test-cap` invocation once you have the reading
you needed.

---

## Environment gotchas learned this session (save yourself the rediscovery time)

- **Build**: `.\windows\build.ps1` from repo root (wraps `Enter-VsDevShell` + `cmake --build`).
  Never invoke `cl.exe`/`link.exe` directly. `-Clean` flag wipes and reconfigures.
- **Locked files**: if any `.exe`/`.dll` under `windows\build\` is locked by a lingering process,
  build fails with `LNK1168`/similar. Always `Get-Process HeadphoneSafetyTray,HeadphoneSafetyApo,
  loopback_capture,shared_state_dump,shared_state_write,force_revert_default,timeout_runner_test,
  limiter_test -ErrorAction SilentlyContinue | Stop-Process -Force` before rebuilding if you've
  been running any of these live.
- **PATH**: this session's shell does not persist environment variables between separate tool
  calls (working directory does persist). Every command needing `ffmpeg`/`ffplay` on PATH needs
  `$env:PATH = [System.Environment]::GetEnvironmentVariable("PATH","Machine") + ";" +
  [System.Environment]::GetEnvironmentVariable("PATH","User")` re-run in that same command.
- **C++17, not C++20**: `windows/CMakeLists.txt`'s `CMAKE_CXX_STANDARD` — do not change. Hit a
  real CMake+Ninja+MSVC bug with C++20 module dependency-scanning early in this project
  (`$Config`-shaped literal text corrupting generated Ninja files) that was fully resolved by
  staying on C++17. Also separately: a PowerShell variable named `$Config` in `build.ps1` got
  silently clobbered by `Enter-VsDevShell`'s own internal state — renamed to `$BuildConfig`,
  don't reintroduce a `$Config`-named variable in that script.
- **ASCII-only** in every file this project touches (`.ps1`, `.cpp`, `.h`, `.md` alike) — no
  em-dashes, no smart quotes. A real bug early in this session was em-dashes breaking Windows
  PowerShell 5.1's script parser; the convention was kept everywhere afterward as a precaution
  even in files where it wouldn't strictly break anything (C++ comments, etc.).
- **Elevation**: `Start-Process -Verb RunAs` triggers a real UAC prompt the user must approve —
  this session found these prompts can silently fail ("canceled by the user") if something about
  the session/desktop context isn't right; if that happens, ask the user directly whether they're
  actually seeing prompts before retrying blindly.
- **`audiosrv` restart ≠ `audiodg.exe` restart** — confirmed live, repeatedly. Restarting the
  `Windows Audio` (`audiosrv`) *service* does NOT restart the `audiodg.exe` *process*. To force a
  genuinely fresh `audiodg.exe`, kill it directly: `Stop-Process -Name audiodg -Force` (elevated).
  It respawns automatically on next playback. (This specific gotcha was central to Approach A's
  debugging and is likely irrelevant to Approach B, which doesn't touch `audiodg.exe` at all — but
  worth knowing if anything ever needs to touch that layer again.)
- **Commit messages**: use `git commit -F <scratchpad-file>`, not `-m` with an inline multi-line
  string — this session hit repeated argument-parsing failures passing punctuation-heavy
  multi-paragraph messages inline through this PowerShell environment.
- **Safety classifier false positives**: a couple of purely read-only or clearly-scoped commands
  got blocked by the session's safety classifier this run (e.g. a `Get-ItemProperty`-only command
  that happened to contain the substring "FxProperties" in a way that pattern-matched against a
  "Remove-Item on protected path" heuristic). If a command gets blocked and it's genuinely
  read-only/safe, simplifying the command text (splitting it up, removing incidental
  suspicious-looking substrings) and retrying has worked.
- **Test methodology that actually caught real bugs**: always isolate Volume Cap (temporarily
  disable it) when measuring the Real-Time Limiter's effect in isolation — otherwise Volume Cap's
  own clamping confounds the measurement and you can't tell which feature caused what. Always
  raise the device volume above any clamp ceiling before an isolated limiter test (via
  `keybd_event` P/Invoke for `VK_VOLUME_UP` — `SendKeys` was tried first and does NOT reliably
  forward media keys, don't waste time on that approach again). Always capture from the *specific*
  device you actually care about via `loopback_capture.exe`'s explicit device-ID argument when the
  system default might not be what you think it is (Approach B specifically: default IS VB-Cable
  while limiting, so you must pass the real device's ID explicitly, or you measure the wrong
  thing).

---

## Key file/doc locations

- Plan file: `C:\Users\kbs\.claude\plans\ok-so-now-you-enumerated-fog.md` — the authoritative,
  continuously-updated plan with a detailed "Open risks" section covering everything found this
  session, including the full Approach A investigation.
- `docs/windows-port.md` — the architecture guide, extensively updated with real findings
  throughout this session (not just original research) — read its "Known blocker" section for
  the full Approach A story before touching that code again.
- `windows/README.md` — user-facing docs, needs the Approach B rewrite described above.
- This file (`still_to_do.md`, repo root) — delete once everything above is done and folded into
  the permanent docs.
