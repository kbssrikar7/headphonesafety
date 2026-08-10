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

- [ ] Volume Cap clamps correctly on a headphone/Bluetooth-classified device, leaves the phone
      speaker untouched.
- [ ] Confirmed which specific apps the Real-Time Limiter actually works with on the test device
      (expect Spotify/Chrome/SoundCloud, at minimum, to be unprocessed per known restrictions) —
      document this list for users rather than implying universal coverage.
- [ ] Confirmed via logged peak values that a deliberately loud/clipping input is actually capped
      at the output for an app that *does* support capture.
- [ ] Toggling the limiter off reverts cleanly and promptly.
- [ ] Force-stopping the app (Settings → Apps → Force Stop) does not leave the system in a broken
      audio state.
- [ ] Physically disconnecting headphones while the limiter is active reverts safely.
- [ ] Confirmed exact behavior/steps required on Android 15+ regarding the "disable screen capture
      protection" Developer Options requirement, and documented them clearly for users.

## Key references

- [Sound dose (Android Open Source Project)](https://source.android.com/docs/core/audio/sound-dose) — Android's native feature, confirms no public API
- [Android Authority: Pixel "Hearing Wellness" feature](https://www.androidauthority.com/google-pixel-hearing-wellness-feature-3590326/)
- [Capture video and audio playback (Android Developers)](https://developer.android.com/media/platform/av-capture) — official `AudioPlaybackCaptureConfiguration` docs
- [Android Developers Blog: Capturing Audio in Android Q](https://android-developers.googleblog.com/2019/07/capturing-audio-in-android-q.html) — original introduction of this API, useful background
- [RootlessJamesDSP (GitHub)](https://github.com/timschneeb/RootlessJamesDSP) — the real-world precedent; GPL-3.0-or-later, study both its source and its documented compatibility list/limitations directly rather than relying on secondhand summaries
- [DynamicsProcessing (Android Developers)](https://developer.android.com/reference/android/media/audiofx/DynamicsProcessing) / [DynamicsProcessing.Limiter](https://developer.android.com/reference/android/media/audiofx/DynamicsProcessing.Limiter) — built-in limiter effect, evaluate against the hand-rolled DSP approach
- [AudioDeviceCallback (Android Developers, via archived docs)](https://webarchive.library.unt.edu/web/20160706144332mp_/https://developer.android.com/reference/android/media/AudioDeviceCallback.html) — headphone/Bluetooth connect-disconnect detection
- [Google Play Developer Content Policy Center](https://play.google/developer-content-policy/) — review current policy on media-capture APIs before assuming Play Store distribution is viable
