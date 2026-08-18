# Headphone Safety for Android — Implementation Guide

## Context: what this is, and an important framing difference from the other ports

This is a port of a macOS menu bar app ("Headphone Safety") that brings iOS's "Reduce Loud
Sounds" behavior to a desktop OS. The macOS version has two independent features:

1. **Volume Cap** — a fast, simple ceiling on the output device's volume level.
2. **Real-Time Limiter** — true peak limiting of the actual audio signal, matching what iOS does.

**Before building anything here, know that Android is a different situation from macOS/Windows/
Linux in one important way: Android is actively adding this exact feature natively, and unlike
the desktop OSes, there is no public API for a third-party app to hook into it.** This changes
what a custom app is actually for on this platform — see the next section.

Also, up front: **of the four platforms researched (macOS, Windows, Linux, Android), Android is
the most restrictive one for the Real-Time Limiter feature**, both technically and in terms of
whether it could be distributed normally via the Play Store. Read this whole document, especially
the "Play Store distribution risk" section, before committing significant effort — the honest
assessment is that this is buildable and has a real precedent, but shipping it to non-technical
users at the polish level of the macOS app is a genuinely harder problem here than on any of the
other three platforms.

### Android already has a native version of this — "Sound Dose" / "Hearing Wellness"

- Android 14 introduced **sound dose** monitoring in the audio framework/HAL: continuous
  monitoring of sound pressure exposure, implementing the IEC 62368-1 / EN 50332-3 regulatory
  standard. It's disabled by default in plain Android 14, and the actual calculation logic only
  shipped starting Android 14 QPR1.
- Android 15 added a **"Hearing Wellness"** settings page (rolling out on Pixel devices, under
  Settings → Sound & vibration) with two toggles: Sound Exposure Notifications and Hearing
  Health. It calculates a 7-day rolling "calculated sound dose" from momentary exposure levels,
  warns the user, and — if ignored — **automatically lowers the volume**, similar in spirit to
  what this app's Volume Cap does, but built into the OS.
- **In some regions, this is forced on and cannot be disabled**, per the underlying regulation.
- **Critically: there is no public API for this.** Google's own documentation states this
  explicitly — the feature has no external-facing APIs, and is configured only by OEMs via HAL
  implementation and system config flags. A third-party app **cannot** query, extend, or integrate
  with it.

**What this means for this project**: a custom Android app can't build *on top of* the OS
feature — it has to be a fully separate, parallel implementation, same as the original macOS
build. Where it might still be worth building: devices/OS versions/regions where sound dose isn't
enabled, users who want more control/customization than the stock toggle offers, or (most
relevantly) **non-Pixel Android devices**, since sound dose's actual enforcement behavior is
OEM-dependent and not guaranteed present on, say, a Samsung, Xiaomi, or OnePlus phone the way
Reduce Loud Sounds is guaranteed present on any iPhone.

---

## Architecture overview

### Feature 1: Volume Cap

Straightforward, same shape as the other three platforms, using `android.media.AudioManager`:

- `AudioManager.getStreamVolume(STREAM_MUSIC)` / `setStreamVolume(STREAM_MUSIC, index, flags)` —
  **note this is an index into a discrete step range, not a dB value directly.** Use
  `getStreamMaxVolume(STREAM_MUSIC)` / `getStreamMinVolume(STREAM_MUSIC)` to get the device's
  actual step range (varies by device/OEM — commonly 15 or 25 steps, but not fixed). There's no
  direct Android API returning "this step is X dB" the way WASAPI does on Windows — if a true
  dB-headroom concept (matching the other three platforms' `Settings.capHeadroomDb` presets) is
  wanted here, it will likely need to be approximated as a percentage-of-max-steps cap instead,
  and documented as an approximation rather than true dB, similar to macOS's `scalarOnly` fallback
  path for devices without native dB support.
- No special permission is needed for basic `STREAM_MUSIC` volume control. (One specific
  documented restriction, unrelated to this use case: volume changes that would also toggle Do Not
  Disturb require separate Notification Policy Access — not relevant to a simple media-volume cap.)
- **Headphone/Bluetooth connect detection**: `AudioManager.registerAudioDeviceCallback()`
  (public API since Android 6.0 / API 23) — implement `onAudioDevicesAdded` /
  `onAudioDevicesRemoved`, check `AudioDeviceInfo.getType()` for `TYPE_WIRED_HEADPHONES`,
  `TYPE_WIRED_HEADSET`, `TYPE_BLUETOOTH_A2DP`, etc. This is the direct equivalent of macOS's
  `kAudioDevicePropertyTransportType` classification and is event-driven, not polling — cleaner
  than the 0.4s poll loop the macOS version uses.

### Feature 2: Real-Time Limiter

**RESOLVED (2026-08-13): unconditional, system-wide parity with iOS is not achievable via public
APIs — but a real, partial-coverage limiter is, and that's what this app builds.** Three candidate
mechanisms were evaluated; two are dead ends, the third is what real shipping apps (Wavelet) use
and what this project builds too, coverage caveats and all — recorded here so a future session
doesn't re-derive the two dead ends:

1. **`AudioPlaybackCaptureConfiguration` capture → DSP → `AudioTrack` replay** (the architecture
   sketched below, and RootlessJamesDSP's apparent shape at a glance). **Confirmed non-destructive
   to the source**: per Google's own `developer.android.com/media/platform/av-capture` docs, the
   original app's audio keeps playing unmodified while your app receives a copy — there is no API
   to mute or suppress the source during capture (`AudioPlaybackCaptureConfiguration.Builder`
   exposes only usage/UID matching, nothing that silences the original). A limiter built this way
   doesn't replace the signal, it plays a second, quieter copy *on top of* the unlimited original —
   strictly worse than doing nothing, not a limiter at all. This is exactly the "does the original
   audio also keep playing" question flagged below as needing resolution before writing limiter
   code; it's now resolved, and the answer kills this architecture.
2. **`AudioEffect`/`DynamicsProcessing` attached to the global output mix (audio session 0)** — the
   classic mechanism older system-wide equalizer apps used, which would be a genuine in-line insert
   effect with no capture/replay round-trip and thus no duplication problem. Confirmed **deprecated
   and "largely non-functional" on modern Android** (deprecated within about a year of Gingerbread,
   c. 2010) — works on some older devices via legacy fallback paths, not something to build a
   current feature on.
3. **Per-app-session `AudioEffect` attachment** — what real apps (Wavelet, and presumably
   RootlessJamesDSP) actually do: attach to an individual app's audio session ID, which *is* a
   genuine non-duplicating insert effect. The catch is discovery — apps aren't required to
   broadcast their session ID, and many modern ones don't. Coverage for non-broadcasting apps
   requires the `android.permission.DUMP` permission (used to read session IDs out of
   `dumpsys media.audio_flinger`), which is a signature/privileged permission a user **cannot**
   grant through a normal permission dialog — only via `adb shell pm grant <pkg>
   android.permission.DUMP` from a computer. Even with that, coverage is per-app and unknowable in
   advance: no way to tell a user in advance which of their apps will and won't be protected. This
   is real-world-functional (it's what ships today) but it is not "exactly like the iOS one" —
   iOS's Reduce Loud Sounds is unconditional and system-wide; this is partial-coverage, and full
   coverage needs `DUMP` granted via a desktop `adb` command, not something available from the
   phone alone.

**Decision: build mechanism 3.** Attach `android.media.audiofx.DynamicsProcessing` (Limiter stage
only) to individual audio sessions, discovered via the `ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION`
broadcast for apps that send it, plus `DUMP`-permission `dumpsys media.audio_flinger` parsing for
apps that don't. Coverage is real but partial and app-dependent — same category of honest caveat
as this project's Linux finding that the limiter's virtual sink is unavoidably visible in the OS
device picker (a platform constraint to document, not a bug to keep hunting a fix for), and the
same category as RootlessJamesDSP's own documented Spotify/Chrome/SoundCloud opt-outs. The app
must surface which apps are actually covered rather than implying universal protection, and must
detect and state whether it currently holds `DUMP` at runtime. The rest of this section below is
kept as historical record of the original (invalidated) capture-and-replay sketch and the
RootlessJamesDSP research that led to mechanism 3.

**Built and live-verified (2026-08-13) on a Samsung Galaxy S9+ (SM-G965F), Android 10 / One UI,
`DUMP` granted via `adb`.** Session discovery works exactly as designed: `dumpsys
media.audio_flinger`'s "Global session refs:" table surfaced real third-party sessions, including
YouTube's — confirmed by cross-referencing `pid`→package via `/proc/<pid>/cmdline`, then watching
the specific session ID attach successfully while a YouTube video played. 7 of 9 discovered
sessions attached without error; 2 (Samsung's on-screen keyboard's click-sound sessions)
consistently fail with a native `Cannot initialize effect engine ... Error: -3` — plausibly because
those are extremely short-lived, non-media sessions the vendor's effect engine doesn't support, not
worth chasing further for a hearing-safety app (keyboard clicks aren't a hearing-safety concern).

Implementation note: constructing `DynamicsProcessing` with an explicit `Config` that hardcodes
`channelCount=2` **crashes with `IllegalArgumentException: bad parameter value`** on any session
whose real channel count doesn't match — the constructor internally calls its own
`getChannelCount()` and validates the supplied `Config` against it, rejecting mismatches outright
(this is what caused 7/9 initial failures before the fix). The reliable pattern: construct with
`cfg = null` (lets the engine auto-detect and default-configure for the session's actual channel
count, which is always valid), then explicitly disable the auto-created preEQ/MBC/postEQ stages
per-channel and configure only the Limiter stage. `android/app/src/main/kotlin/com/
headphonesafety/android/SessionLimiterManager.kt` implements this.

**Vendor-specific hard limit found via readback verification, not assumed:** on this device,
`DynamicsProcessing.Limiter.setThreshold()` silently does not take effect — read back
immediately after setting, and via a separate `updateThreshold()` poll cycle, the threshold is
**hard-pinned to -2.0 dB** no matter what value is requested (tested -10, 0, and -20 dB; all read
back -2.0). This is Samsung's `libdynproc.so` HAL implementation, not an app-code bug — the same
"a successful call is not proof it took effect" lesson this project has hit on every platform,
this time surfacing as a parameter that silently clamps rather than a routing change that silently
no-ops. Net effect: on this specific device, the headroom preset the user picks for the limiter
does not currently change limiting behavior — the effect stays attached at a fixed threshold
readback of -2.0 dB, confirmed genuinely live in the audio graph (not just a successful API call —
see the effect-chain diff below), but not the adjustable-headroom behavior the UI advertises. The
actual acoustic attenuation this fixed threshold produces hasn't been independently measured — the
chain-diff confirms activation, not magnitude; don't read "-2.0 dB readback" as "confirmed -2 dB of
real limiting." Unconfirmed whether this is Samsung-wide,
One-UI-version-specific, or would behave differently on Pixel/AOSP-reference or other OEM
implementations — flagged in `android/README.md` as a known, honestly-documented device-dependent
limitation rather than something the app can control from userspace.

**Measurement harness (2026-08-18): built, and its failure mode taught more than a working
harness would have.** To test whether the -2dB clamp above could be routed around via other
`DynamicsProcessing` stage parameters (`postGain`, MBC, PreEq), a debug-only in-app measurement
tap was built (`PlaybackCaptureHarness.kt` + `HarnessCaptureService.kt`, gated behind
`BuildConfig.DEBUG`): `AudioPlaybackCaptureConfiguration` capturing this app's own `USAGE_MEDIA`
test tone, computing peak/RMS dBFS. **Control test result: this tap cannot measure per-session
effects on this device at all.** A 0 dBFS test tone at max stream volume captured at a fixed
**-18.06 dB peak** regardless of whether the Limiter was attached-and-enabled or fully released —
and, decisively, regardless of whether an `Equalizer` with all 5 bands pinned to their minimum
gain (-15 dB each, confirmed `enabled=true` readback on the tone's *own* `getAudioSessionId()`,
not a decoy session) was attached. -18.06 dB is suspiciously exact: `20*log10(4096/32767) =
-18.06`, i.e. sample value 4096 = 2^12 = exactly 1/8 full scale — a fixed digital pad the capture
path applies, unrelated to the actual acoustic signal. The tap sits upstream of wherever
per-session insert effects apply, so no capture-level comparison through it is meaningful for any
session effect, Limiter or otherwise. (A 3.5mm-out → laptop line-in fallback was also attempted
and abandoned — the laptop's analog input never received signal from the phone regardless of
volume/gain-boost settings, most likely a TRS/TRRS pin mismatch in the laptop's combo jack; not
worth further hardware debugging given the finding below made it unnecessary.)

**The discriminating test that mattered: `dumpsys media.audio_flinger`'s per-session effect-chain
listing, diffed before/after attaching an effect.** With the `Equalizer` above attached and
enabled on the tone's live session (id 40729 in this run), the dump showed `2 effects for session
40729`, including an entry for the Equalizer with `Registered=y Enabled=y Suspended=n` and its
real NXP-software engine handle. Releasing it dropped the listing to `1 effects for session
40729` and the Equalizer entry vanished entirely. **This is hard, direct evidence that per-session
effects are genuinely inserted into and removed from AudioFlinger's live processing graph on this
device** — the Real-Time Limiter architecture is validated as functional at the audio-engine
level; the earlier capture-harness null result was an instrument-blindness artifact, not evidence
the Limiter (or any session effect) is a no-op. This chain-diff technique — not the capture
harness — is the reliable way to confirm "is this effect actually active," and is what the clamp-
workaround arms (postGain/MBC/PreEq) use going forward, combined with parameter readback (to catch
a silent clamp like the threshold one above) and, where a human is available, an audible check
(the Equalizer min-gain test is unmistakably audible, confirming the chain-diff's read of "active"
matches what a listener actually hears).

**Clamp-workaround arms (2026-08-18): all three fail identically — no adjustable
`DynamicsProcessing` dB path exists on this device.** With the chain-diff instrument validated
above, all three arms from the original plan were tested directly in `SessionLimiterManager.attach()`
against the live test-tone session, each read back immediately after being set:

- **Arm 1 — `Limiter.setPostGain(-20f)`**: requested -20.0, **readback 0.0**.
- **Arm 2 — MBC re-enabled, permissive threshold (0dB)/ratio (1:1) so behavior is dominated by
  gain, `MbcBand.setPostGain(-20f)` on every band** (real `bandCount` read off the config, not
  assumed — 6 bands on this device): requested -20.0, **readback 0.0 on every band**.
- **Arm 3 — PreEq re-enabled, `EqBand.setGain(-20f)` on every band** (6 bands): requested -20.0,
  **readback 0.0 on every band**.

Every arm was silently reset to its inert default (0.0), the exact same failure shape as the
Limiter threshold clamp above (request an extreme value, read back the untouched default) —
not a crash, not an exception, not even a stored-but-inert value; the parameter simply never
moved. **Honest conclusion, per the plan's own stopping rule: this Samsung `libdynproc.so` HAL
implementation exposes no adjustable gain/threshold parameter anywhere in `DynamicsProcessing`
that userspace can actually move** — `setEnabled()` is real and does insert/remove the effect
from the live audio graph (proven above), but every numeric parameter that would change *how
much* it does is clamped back to a fixed default. The Real-Time Limiter's only controllable
behavior on this device is therefore binary — on or off — with the threshold parameter permanently
reading back -2.0 dB whenever it's on; that reading is confirmed live in the audio graph, not its
actual acoustic attenuation, which hasn't been independently measured. No adjustable headroom
exists — which is what `android/README.md` and the unified-toggle design (step 4) must state
plainly rather than imply a working dB slider. This experiment code was reverted after
testing (each arm modified and rebuilt `SessionLimiterManager.attach()` in place, one at a time,
then restored to the clean committed baseline) rather than left in the shipped path, since none of
the three worked.

**Unified toggle (2026-08-18): the two-switch UI was retired once the clamp finding made it
misleading.** With no adjustable `DynamicsProcessing` parameter confirmed to exist on any tested
device, a separate "Limiter headroom" dB picker that visibly did nothing was worse than no control
at all. Volume Cap and the Real-Time Limiter now share a single "Enable Headphone Safety" toggle
and one headroom percentage (`Prefs.unifiedEnabled`/`unifiedHeadroomPercent`) — Volume Cap uses the
percentage directly; the Limiter attaches automatically whenever the toggle is on, at its fixed,
device-determined ceiling, unaffected by the percentage. `Prefs.migrateIfNeeded()` is a
schema-versioned, idempotent migration called from `MainActivity`, `VolumeCapService`, and
`BootReceiver`: `unified_enabled = wasCapEnabled || wasLimiterEnabled` (never silently drops
protection), `unified_headroom_percent` is seeded from the legacy `headroom_percent` key **alone**
— not `max(headroom_percent, limiter_headroom_db)` as the original plan sketched, since the
limiter value is confirmed inert and folding a dead number into `max()` could silently overwrite a
user's real, working Volume Cap percentage with a number that never did anything. Legacy keys are
left in place, unused, for rollback safety.

Verified two ways on the S9+: **upgrade-in-place** (installed the pre-unification build, set real
legacy prefs via its old two-switch UI — `headroom_percent=15, volume_cap_enabled=false,
limiter_enabled=true` — then installed the migrated build over it and confirmed
`unified_enabled=true, unified_headroom_percent=15` exactly as designed, UI included), and a **real
device reboot** (with the user's advance consent) confirming `BootReceiver` resumes the service
using the migrated key with zero manual app launch. The reboot test surfaced a real platform
behavior worth recording: the foreground service did not reappear immediately after boot —
`BOOT_COMPLETED` for this non-direct-boot-aware app was held back until the *first post-reboot
unlock*, not delivered at boot time itself, on this file-based-encrypted device. `adb shell` cannot
simulate `BOOT_COMPLETED` at all (`Security exception: Permission Denial`), so this could only be
confirmed via an actual reboot — a good example of why this project's live-device-testing
discipline keeps finding things a code read wouldn't.

---

This is the hard part, and the one place Android is meaningfully more restricted than the other
three platforms.

**The core problem**: a normal third-party app cannot attach a DSP effect to the *whole system's*
audio output the way a macOS AudioServerPlugIn or a Windows APO can (with the caveats those had
too, but Android's version of this problem is worse — see below). The closest thing Android has
publicly is capturing *other apps'* playback via `AudioPlaybackCaptureConfiguration`
(introduced Android 10 / API 29), processing it, and playing the processed result back via a
normal `AudioTrack` — conceptually the same "capture → process → re-render" shape as the macOS
BlackHole architecture, but built entirely differently and with real, hard restrictions macOS
didn't have.

**Real-world precedent — study this first**: [RootlessJamesDSP](https://github.com/timschneeb/RootlessJamesDSP)
is a real, actively maintained, open-source (GPL-3.0-or-later) app doing exactly this class of
system-wide audio DSP on non-rooted Android devices. Read its source and its documented
limitations closely — they are the actual, current constraints of this approach, not
hypothetical ones:

1. **Not all apps can be processed.** Any app can opt out of having its audio captured, via
   `AudioAttributes.Builder.setAllowedCapturePolicy(ALLOW_CAPTURE_BY_NONE)` (or the manifest-level
   equivalent) — and several major, extremely commonly-used apps do exactly this, including
   **Spotify** (unpatched), **Google Chrome**, and **SoundCloud**, per RootlessJamesDSP's own
   documented compatibility list. Audio from these apps will simply not be processed at all —
   there is no workaround short of the *target* app being modified (RootlessJamesDSP's own
   community works around this for Spotify specifically via a ReVanced patch, which is
   modifying Spotify's APK — a different, separate can of worms, not something this project should
   assume it can rely on). **This is a real, meaningful gap**: a hearing-safety app that silently
   doesn't work on some of the most popular streaming apps is a significant caveat to be upfront
   about, not hide.
2. **Only certain audio "usages" are capturable at all**, regardless of opt-out: the played audio's
   `AudioAttributes.usage` must be `USAGE_MEDIA`, `USAGE_GAME`, or `USAGE_UNKNOWN`. This is
   probably fine for the media/video/music use case this app cares about, but voice calls,
   notification sounds, etc. are out of scope structurally, not just by choice.
3. **Android 15+ requires manually disabling "screen capture protection" in Developer
   Options** for this to work at all. This is a real, serious adoption barrier — Developer Options
   is hidden behind a "tap build number 7 times" easter egg and is squarely power-user territory;
   this is not something a normal, non-technical user (the actual target audience for a hearing-
   safety app) can reasonably be expected to do. Any version of this app targeting Android 15+
   needs to either build a very clear in-app guided setup flow for this, or accept that its
   practical audience is limited to technically comfortable users.
4. **`DynamicsProcessing`-based effect apps cannot coexist with each other.** Only one such app's
   effect can be active system-wide at a time — if the user has another system-wide EQ/DSP app
   (Wavelet, RootlessJamesDSP itself, etc.) installed and active, expect a real conflict, not just
   a theoretical one.
5. **Increased latency**, per RootlessJamesDSP's own documented constraints — expected for this
   architecture, same category of cost as the macOS BlackHole round-trip, but worth setting
   expectations about up front.
6. Apps using the native **AAudio C++ API directly** (bypassing the Java `MediaPlayer`/
   `AudioTrack` layer) reportedly can't be patched/captured either, per RootlessJamesDSP's notes —
   worth re-verifying against the current version of their docs when this is actually built, since
   this kind of platform detail shifts across Android releases.

**Implementation sketch, following the same general shape as RootlessJamesDSP:**

1. Request a `MediaProjection` token (`MediaProjectionManager.createScreenCaptureIntent()`) — yes,
   this is the *screen recording* consent API, reused for audio-only capture. The user will see a
   system dialog that looks like a screen-recording permission prompt, even though no video is
   involved. This is worth calling out explicitly in the app's own UI/onboarding, since it will
   otherwise look alarming/confusing ("why does a hearing-safety app want to record my screen?").
2. Build an `AudioPlaybackCaptureConfiguration` from that `MediaProjection`, restricted to
   `USAGE_MEDIA`/`USAGE_GAME` (per the capturable-usages restriction above).
3. Create an `AudioRecord` using that capture configuration to receive the mixed audio from other
   (non-opted-out) apps.
4. Apply the limiter DSP (see below) to the captured buffers.
5. Play the processed result back via a normal `AudioTrack` targeting the real output device.
6. **The original, unprocessed audio is likely still playing simultaneously from its source app**
   unless something suppresses it — verify exactly how RootlessJamesDSP handles this (it's not a
   simple "capture replaces playback" model on Android the way BlackHole-as-default-output was on
   macOS; there's no OS-level "route this app's output somewhere else instead" concept exposed
   publicly). This is a structural question worth resolving with certainty before writing much
   code — it may be that Android's playback-capture model is inherently non-destructive (both the
   original and the capture happen), which would mean this architecture needs a different
   approach than "replace" — possibly capturing *is* effectively exclusive in practice for apps
   that support it, but this needs to be confirmed against RootlessJamesDSP's actual behavior/
   source rather than assumed.

### Limiter DSP algorithm

Same algorithm as the Windows and Linux ports — this part of the problem is platform-independent
DSP, not Android-specific:

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

Android does ship a built-in `android.media.audiofx.DynamicsProcessing` effect (API 28+) with a
proper limiter stage (`DynamicsProcessing.Limiter`) that could be used instead of hand-rolling the
above — but given the "can't coexist with other DynamicsProcessing-based apps" constraint noted
above, and that it's normally designed to attach to a specific audio *session* rather than an
arbitrary captured buffer, evaluate carefully whether it can actually be applied to the
`AudioRecord` buffer from step 3-4 above, or whether a hand-rolled limiter applied directly to the
raw PCM buffer is more reliable. Worth prototyping both and comparing before committing to one.

---

## Play Store distribution risk — not a hard "no," but genuinely uncertain

This was researched twice, and the honest state of it is: **not confirmed impossible, but not
confirmed clean either.** Be precise about what's solid versus what isn't before deciding whether
to pursue a Play Store listing:

**Reasonably solid**:
- No explicit rule was found anywhere in Google Play's Developer Content Policy banning this
  category of app outright.
- Audio-capture-adjacent permissions fall under Google's **restricted permissions** system, which
  has a real, defined process: a **Permissions Declaration Form** in Play Console, where the
  developer justifies why the permission is core to the app's functionality (often requiring a
  video demonstration), reviewed case by case — not an automatic rejection.

**Not solidly confirmed, flag as still open**:
- One research pass turned up a claim that Google explicitly recognizes real-time,
  capture-then-immediately-reprocess-then-output use cases (e.g. live captioning) as legitimate
  precedent for this class of permission — which would be a strong positive signal, since it's
  structurally identical to what this app does (no recording, no storage, no redistribution).
  **A follow-up attempt to verify this directly against Google's primary policy pages could not
  confirm it cleanly** — get a definitive read on this specifically (ideally by reading Google's
  current Permissions Declaration Form guidance and restricted-permissions policy pages in full,
  not just a search summary) before relying on it as a reason to expect approval.
- Google's own API documentation does contain language restricting capture of "copyrighted music or
  audio from movies and TV shows" in some contexts — whether that specific restriction actually
  applies to a real-time-process-and-immediately-replay use case (as opposed to recording/storing)
  was not confirmed either way.

**The one concrete, unambiguous signal**: **RootlessJamesDSP itself, the closest real precedent, is
not distributed via the Play Store** — GitHub Releases and F-Droid only. That's a real data point,
though it's still just a signal, not proof of rejection — it could equally reflect a deliberate
choice to avoid the review process rather than a failed attempt.

**Practical implication**: don't assume either outcome. If Play Store distribution matters, budget
for actually going through the Permissions Declaration Form process and finding out directly,
rather than assuming rejection - but plan for sideloaded APK / F-Droid distribution as the safe
fallback path (mirroring both RootlessJamesDSP's actual choice and this project's own macOS
distribution model, which is also outside an app store) unless a Permissions Declaration Form
review specifically confirms Play Store approval before investing in a store listing.

---

## Hard-won lessons from the macOS build (read before writing rollback/detection logic)

The macOS build went through multiple live-tested rounds where a seemingly-safe revert mechanism
turned out not to be safe in practice. These are the actual root causes found — treat all of them
as *generically likely to recur* here, even though Android's APIs are completely different in
shape:

1. **A "successful" API call is not proof the change took effect.** On macOS, a device-routing
   change silently no-op'd while still reporting success. Any Android equivalent (volume set
   calls, capture setup, etc.) should be **verified by reading the actual resulting state back**
   where possible, not trusted from a callback/return value alone.

2. **There is a real timing window right after a device disconnects where operations can
   transiently fail, even targeting something that's still valid.** Design any recovery logic to
   **retry until it actually lands**, not attempt once and give up.

3. **Real-time audio callback threads must never block or do expensive/synchronous work.** This
   applies directly to Android too: whatever code processes the captured `AudioRecord` buffer and
   feeds the `AudioTrack` runs on a real-time-sensitive path — avoid allocation, locks, or I/O in
   that path, the same discipline the macOS `AURenderCallback` code followed. A blocking call here
   won't just glitch audio, it can cascade into UI-thread or watchdog freezes if anything shares a
   lock or queue with it, exactly as happened on macOS.

4. **A device/state-change listener should react unconditionally, not re-verify by querying** in
   a way that could itself block or race. When `AudioDeviceCallback.onAudioDevicesRemoved` fires
   for the tracked headphone device, treat it as "stop and revert" immediately rather than
   re-querying current device state first.

5. **Test the actual failure scenario live, not just the happy path.** Every fix on the macOS
   build was found by physically disconnecting real Bluetooth headphones while the feature was
   active and watching what actually happened, not by reasoning about API docs alone. Budget real
   device testing time here too: toggle on/off, force-stop the app while the limiter is active,
   and physically disconnect Bluetooth headphones mid-playback — each followed by confirming audio
   actually behaves correctly afterward, not just that no exception was thrown.

6. **A plain process kill/quit doesn't always trigger the same clean shutdown path as a normal
   user-initiated stop.** Found late in the macOS build: `kill <pid>` (SIGTERM) did not reliably
   trigger the same revert logic that the in-app Quit menu item did — a real, distinct code path
   that needs its own explicit testing, not an assumption that "graceful shutdown" always fires.
   On Android, the equivalent risk is a foreground service being killed by the system (low memory,
   battery optimization, "force stop" from Settings) — test each of these specifically, don't
   assume `onDestroy()`/similar lifecycle callbacks are a reliable place to put the only cleanup
   logic.

---

## Recommended build order

1. **Volume Cap only**, using `AudioManager` + `AudioDeviceCallback` for headphone detection. Low
   risk, no special permissions, and it's the fallback behavior for the harder feature.
2. **MediaProjection + AudioPlaybackCaptureConfiguration pass-through, no DSP.** Get captured audio
   flowing through to an `AudioTrack` completely unmodified first, and resolve the "does the
   original audio also keep playing" structural question (see above) before writing any limiter
   code — this is foundational, not a detail to defer.
3. **Add the limiter DSP**, verify audibly and via logged peak values that it's a true peak
   limiter, not just quieter audio.
4. **Rollback and safety testing**, per the lessons above — toggle-off, app force-stop, headphone
   disconnect, each tested live against real hardware.
5. **UI**: a foreground service + notification (required for any long-running audio-processing
   background work on modern Android) with a simple settings screen, mirroring the macOS app's
   menu structure — device status, cap toggle + headroom, limiter toggle + status, plus a clear
   explanation of the MediaProjection consent prompt shown during onboarding.

## Testing checklist

This checklist predates the Feature 2 architecture pivot (see the "RESOLVED" note above) — some
items below (Android 15+ screen-capture Developer Options, MediaProjection-specific disconnect
behavior) were written for the invalidated capture-and-replay design and no longer apply to the
per-session `DynamicsProcessing` mechanism actually built. Superseded items are marked as such
rather than deleted, so the original scope stays visible.

- [x] Volume Cap clamps correctly on a headphone/Bluetooth-classified device, leaves the phone
      speaker untouched. Verified live on a Galaxy S9+ (Android 10) with a real Sony WH-CH720N over
      Bluetooth: 20 volume-up presses stopped at exactly `floor(15 × (100−10)/100) = 13`, and with
      no headphones connected the same 20 presses left volume unchanged.
- [x] Confirmed which specific apps the Real-Time Limiter actually works with on the test device —
      verified live against YouTube (attached successfully, cross-referenced pid→package via
      `/proc/<pid>/cmdline`); 7 of 9 total discovered sessions attached, 2 (the on-screen keyboard's
      click sounds) failed with a native engine-init error. Spotify/Chrome/SoundCloud not
      specifically tested this session — same category of expected gap RootlessJamesDSP documents,
      not yet individually confirmed here.
- [x] Confirmed session effects are genuinely active in the real audio path, not just
      accepting parameters — via `dumpsys media.audio_flinger`'s per-session effect-chain diff
      (see the "Measurement harness" note above), not via logged peak values: `AudioPlaybackCaptureConfiguration`-based
      capture was tried first and found to sit upstream of session effects on this device (a fixed
      -18.06dB pad, unrelated to the actual signal, regardless of what's attached), so it can't be
      used for level measurement here. The chain-diff instead directly confirms
      `Registered=y Enabled=y Suspended=n` appearing/disappearing for an attached effect. Still
      open: a dB-accurate measurement of the Limiter's specific numeric effect (the chain-diff
      proves activation, not magnitude) — would need a working analog capture path, not yet
      achieved on this device (3.5mm-out → laptop line-in was attempted and abandoned; the
      laptop's input never received signal, likely a TRS/TRRS mismatch in its combo jack).
- [x] Toggling the limiter off reverts cleanly — confirmed via `SessionLimiterManager.releaseAll()`
      being called whenever `DUMP` isn't held or the feature is disabled; each attached
      `DynamicsProcessing` instance is disabled then released, not just abandoned.
- [ ] Force-stopping the app (Settings → Apps → Force Stop) does not leave the system in a broken
      audio state. Not yet tested.
- [ ] ~~Physically disconnecting headphones while the limiter is active reverts safely~~ — N/A to
      the mechanism actually built: the limiter attaches per-session, not via a routing change tied
      to the headphone device, so there's no equivalent "revert routing" step the way Volume Cap or
      the macOS/Linux limiters need. Headphone disconnect only matters to Volume Cap, which already
      reacts to it via `AudioDeviceCallback.onAudioDevicesRemoved`.
- [ ] ~~Confirmed exact behavior/steps required on Android 15+ regarding the "disable screen capture
      protection" Developer Options requirement~~ — N/A; this was specific to the invalidated
      MediaProjection-based design, not the per-session mechanism actually built.

## Key references

- [Sound dose (Android Open Source Project)](https://source.android.com/docs/core/audio/sound-dose) — Android's native feature, confirms no public API
- [Android Authority: Pixel "Hearing Wellness" feature](https://www.androidauthority.com/google-pixel-hearing-wellness-feature-3590326/)
- [Capture video and audio playback (Android Developers)](https://developer.android.com/media/platform/av-capture) — official `AudioPlaybackCaptureConfiguration` docs
- [Android Developers Blog: Capturing Audio in Android Q](https://android-developers.googleblog.com/2019/07/capturing-audio-in-android-q.html) — original introduction of this API, useful background
- [RootlessJamesDSP (GitHub)](https://github.com/timschneeb/RootlessJamesDSP) — the real-world precedent; GPL-3.0-or-later, study both its source and its documented compatibility list/limitations directly rather than relying on secondhand summaries
- [DynamicsProcessing (Android Developers)](https://developer.android.com/reference/android/media/audiofx/DynamicsProcessing) / [DynamicsProcessing.Limiter](https://developer.android.com/reference/android/media/audiofx/DynamicsProcessing.Limiter) — built-in limiter effect, evaluate against the hand-rolled DSP approach
- [AudioDeviceCallback (Android Developers, via archived docs)](https://webarchive.library.unt.edu/web/20160706144332mp_/https://developer.android.com/reference/android/media/AudioDeviceCallback.html) — headphone/Bluetooth connect-disconnect detection
- [Google Play Developer Content Policy Center](https://play.google/developer-content-policy/) — review current policy on media-capture APIs before assuming Play Store distribution is viable
