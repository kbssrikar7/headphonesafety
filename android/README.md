<div align="center">
  <img src="../docs/icon.png" alt="Headphone Safety icon" width="128" height="128">

  # Headphone Safety (Android)

  Real-time headphone volume/loudness protection for Android, bringing as much of iOS's "Reduce
  Loud Sounds" behavior to the platform as its public APIs actually allow.

  ![Platform](https://img.shields.io/badge/platform-Android%208.0%2B-blue)
  ![Kotlin](https://img.shields.io/badge/Kotlin-1.9-orange)
  ![License](https://img.shields.io/badge/license-MIT-green)
</div>

---

> This is the Android implementation. See the [repo root](../README.md) for the other platforms
> (macOS and Linux are also implemented; Windows is an architecture guide only, not yet built).

## Status

Two real devices have been used for live testing: a **Samsung Galaxy S9+ (Android 10, API 29, One
UI)** and a **Samsung Galaxy S21 (Android 15, API 35, One UI 7)** — deliberately chosen as a big
version/hardware-generation gap rather than an emulator, once cross-device compatibility became a
goal. Both are Samsung; see [Device compatibility](#device-compatibility) below for what that does
and doesn't tell us about other manufacturers.

**Volume Cap: built and fully live-verified.** A foreground service caps `STREAM_MUSIC` volume
whenever a headphone- or Bluetooth-classed audio device is present
(`AudioManager.registerAudioDeviceCallback`), leaving the phone speaker untouched. Both halves
confirmed live on the Galaxy S9+ (Android 10): with no headphones connected, 20 consecutive
volume-up presses left the stream volume unchanged (9/15, untouched); with real Bluetooth
headphones connected (a Sony WH-CH720N, `Devices: bt_a2dp` confirmed via `dumpsys audio`), the same
20 presses stopped exactly at 13/15 — `floor(15 × (100−10)/100)`, the expected cap at the 10%
headroom preset, matching the math precisely rather than just "getting quieter."

Two safety nets beyond the obvious "call `setStreamVolume`": the clamp is triggered both by the
(undocumented) `android.media.VOLUME_CHANGED_ACTION` broadcast *and* an independent ~400ms poll
loop, so a missing broadcast on some OEM skin doesn't silently disable the feature; and every
`setStreamVolume` call is followed by a read-back with bounded retries rather than trusted blindly
— the same "a successful call is not proof it took effect" discipline every other platform in this
repo has needed at least once.

**Real-Time Limiter: built, live-verified as functional, with an honestly-documented device-side
limitation.** Unlike macOS and Linux, Android has **no public API for an unconditional,
system-wide, true peak limiter** — this was researched thoroughly (three candidate mechanisms
evaluated, two are dead ends) before writing any code; see
[`docs/android-port.md`](../docs/android-port.md)'s "RESOLVED" note on Feature 2 for the full
writeup. What's actually built: `android.media.audiofx.DynamicsProcessing`'s Limiter stage attached
per audio session, with sessions discovered via `dumpsys media.audio_flinger` (the same mechanism
real apps like [Wavelet](https://github.com/pittvandewitt/Wavelet) use). This is genuinely
functional — live-verified attaching to real active sessions on both test devices, including a real
third-party app (YouTube) confirmed via `pid`→package cross-reference — but coverage is **partial
and app-dependent**, not universal:

- Needs the `android.permission.DUMP` permission, a signature permission Android will not let a
  user grant through a normal in-app dialog — see [Setup](#setup-for-the-real-time-limiter) below.
- Not every session can be attached to; on the S9+, 2 of 9 (both belonging to the on-screen
  keyboard's click-sound effects) fail with a native engine-init error and are silently skipped —
  not a hearing-safety concern, but a real gap the app doesn't paper over.
- **Confirmed via read-back, not assumed, on two different Samsung devices**: the limiter's
  threshold is **hard-pinned to -2 dB by the vendor's own DSP implementation**, regardless of the
  headroom preset selected in the app — tested requesting -10, 0, and -20 dB on the S9+, all three
  read back as -2.0 dB; the same clamp was independently re-confirmed on the S21. This means the
  effect stays genuinely live in the audio graph at that fixed threshold (confirmed via a
  `dumpsys media.audio_flinger` effect-chain diff, not just a successful API call — see below), but
  the headroom preset picker doesn't currently change that ceiling on either Samsung device tested,
  and its actual acoustic attenuation at -2 dB hasn't been independently measured (activation is
  confirmed, magnitude is not).
  This is now confirmed **Samsung/One UI-wide** (two different chip/Android-version generations),
  not a one-device fluke — but still unconfirmed for non-Samsung OEMs (Pixel/AOSP, Xiaomi, etc.),
  since no such device has been available to test against.
- **All three candidate workarounds for the -2 dB clamp tested and failed identically — confirmed
  no adjustable `DynamicsProcessing` dB path exists on this hardware at all.** Tried, each read
  back immediately after being set: `Limiter.setPostGain(-20)` (readback 0.0), MBC re-enabled with
  a permissive threshold/ratio and `setPostGain(-20)` on every band (readback 0.0 on all 6 bands),
  and PreEq re-enabled with `setGain(-20)` on every band (readback 0.0 on all 6 bands). Every
  attempt silently reset to its inert default — same failure shape as the threshold clamp above.
  `setEnabled()` itself is real (confirmed via a `dumpsys media.audio_flinger` effect-chain
  before/after diff — the effect genuinely appears in and disappears from the live audio graph),
  but no numeric parameter that would change *how much* limiting happens can be moved from
  userspace on this device. See `docs/android-port.md`'s "Clamp-workaround arms" note for the full
  methodology and why the original `AudioPlaybackCaptureConfiguration`-based measurement harness
  had to be abandoned first (it sat upstream of session effects entirely, an instrument-blindness
  issue independent of this finding).
- **Device-capability check added**: not every phone's audio HAL ships a `DynamicsProcessing`
  implementation at all — the app now queries this upfront (`AudioEffect.queryEffects()` for the
  effect's type UUID) and shows "Real-Time Limiter isn't available on this device" instead of
  silently accumulating per-session attach failures forever when it's absent.
- **Session-discovery format hardening, driven by a real cross-device bug**: `dumpsys
  media.audio_flinger`'s "Global session refs:" table (the mechanism above) is undocumented debug
  output, not a stable API. Comparing the S9+ and S21 live found its column layout actually
  differs between them — `session pid count` (3 columns) vs `session cnt pid uid name` (5 columns,
  a trailing non-numeric name field). A naive fixed-column parser matches the first and silently
  returns nothing on the second. The parser now reads the header row's column *names* to find
  "session"'s position dynamically instead of assuming a fixed shape, and distinguishes "the table
  section is missing entirely" (a bigger format break, falls back to a looser text scrape and
  surfaces "best-effort detection" in the status text) from "the table's just empty right now"
  (nothing playing). Also found live on the S21: Samsung's own SoundAlive service occupies a
  reserved negative session id (`-3`) in that table — a real, well-formed row, not a parse error —
  which the app now filters out instead of wasting an attach attempt and a caught exception on it.
  Both fixes are covered by JUnit tests (`app/src/test/kotlin/.../SessionLimiterManagerTest.kt`)
  built from the real captured dumpsys text of both devices.

This is the same category of honest, load-bearing caveat as this repo's Linux finding that the
Real-Time Limiter's virtual sink is unavoidably visible as another output device — a real platform
constraint to document clearly, not a bug to keep chasing a fix for from application code.

**Background survival: built and partially live-verified.** Two things had to be true for this app
to actually keep protecting hearing while backgrounded, not just while the app is open:

- **Cold-launch / system-kill auto-resume.** Previously, if Android killed the background service
  (low memory, "Force Stop", etc.), re-opening the app left the saved preferences saying "enabled"
  while nothing was actually running — a real gap, since the earlier live testing sessions all
  happened to start from a fresh manual toggle. Fixed and verified live: force-stopped the app via
  `am force-stop`, confirmed the service was gone (`dumpsys activity services` showed nothing),
  relaunched the activity, and confirmed the service auto-started with no manual toggle needed.
- **Reboot auto-resume**, via a `BOOT_COMPLETED` broadcast receiver (`BootReceiver.kt`) that
  restarts the service if either feature was enabled. Confirmed *registered* correctly (`dumpsys
  package` shows the receiver with the right intent filter) — **not yet confirmed by an actual
  reboot**: `BOOT_COMPLETED` is a protected broadcast `adb shell` isn't allowed to simulate on this
  device, and the code wasn't run through a real reboot this session (a phone reboot wasn't
  something to do without asking first). The receiver's logic mirrors the already-verified
  cold-launch path, but "the same code that's proven to work elsewhere" is not the same claim as
  "proven to work here" — flagged honestly rather than assumed.

### Background survival: deeper platform research

Beyond the two fixes above, five more platform-level questions determine whether this app actually
keeps running unattended — each checked directly rather than assumed, since "should work per the
docs" and "confirmed working on this device" have not been the same thing anywhere in this repo:

1. **Standard Android battery optimization (Doze/App Standby)** — what the in-app "Exempt from
   battery optimization" button controls. Verified: on this device, currently **not** in the
   exemption list by default, and the button correctly opens Samsung's real settings screen for it
   (`Settings$HighPowerApplicationsActivity`, confirmed via `dumpsys window`).
2. **OEM-specific battery/autostart managers separate from standard Android** (Device Care on
   Samsung, but every major manufacturer has its own equivalent) — **not the same mechanism as
   #1**, not present in stock Android, and there is **no programmatic way to open any of them or
   detect an app's status in them** — confirmed for Samsung via
   [dontkillmyapp.com/samsung](https://dontkillmyapp.com/samsung), which states plainly "no known
   solution on dev end"; the same holds for the other manufacturers below per the same site. Only
   Samsung's has been live-verified (Galaxy S9+ and S21, both confirmed present); the app now shows
   **manufacturer-specific guidance at runtime** (`Build.MANUFACTURER`-selected) for Xiaomi, Huawei,
   OnePlus, Oppo/Realme, and Vivo sourced from dontkillmyapp.com and explicitly labeled
   *"not verified on this build"* in the UI text itself, plus a generic fallback note and a link to
   dontkillmyapp.com for anything else — see `MainActivity.buildOemBatteryNote()`. On Samsung,
   confirmed live: **Settings → Device care → Battery → Background usage limits → Never sleeping
   apps → add Headphone Safety.** Apps unused for ~3 days go to "sleeping," ~16 days to "deep
   sleeping" (background work fully blocked until manually reopened). This app's in-app battery
   button does *not* cover any of these OEM managers — it's a real gap between what the app can
   automate and what actually needs doing, now documented instead of silently missed.
3. **Doze CPU throttling of the poll loops** — tested live: turned the screen off for 50 seconds
   with the service running, and the Real-Time Limiter's 2-second poll loop kept ticking on a
   consistent ~2.05–2.15s cadence throughout (checked via logcat timestamps), no gaps. Foreground
   services are architecturally exempted from standard Doze CPU restrictions, and this confirms it
   held on this device for the tested duration — not a multi-hour/overnight test.
4. **Task removal (swiping the app away in Recent Apps)** — a common failure point on many Android
   OEM skins. Tested live: opened Recents, swiped the app's card away, confirmed via `dumpsys
   activity recents` that the task was actually gone — and the foreground service was **still
   running** (`dumpsys activity services` showed it alive). Samsung's One UI does not kill a
   properly-declared foreground service on task swipe on this device.
5. **Android's "unused apps" automatic permission reset/hibernation** (apps unopened for ~3 months
   have permissions revoked and background work stopped) — a stock Android privacy feature, not
   Samsung-specific. Low risk here specifically because an *active foreground service* is itself
   ongoing app usage, which should keep the app out of the "unused" bucket — but if both features
   are switched off *and* the app isn't opened for months, the `DUMP` grant could theoretically be
   swept up in this along with everything else. Not tested (would require actually waiting months);
   noted as a real but low-probability edge case rather than untested-and-unmentioned.

Net: two of these five are genuinely solid (#3, #4, both live-verified with no caveats found), one
is handled but only automatable up to a point (#1), one has no automation path and needs a
documented manual step (#2), and one is a low-probability, unverified edge case (#5).

### Device compatibility

`minSdk` is 26 (Android 8.0), which covers essentially the entire active Android install base — not
lowered further, since `foregroundServiceType`, adaptive icons, and `DynamicsProcessing` (API 28,
already runtime-guarded) all depend on platform support at or above that floor. Beyond the SDK
floor, honest tiers of what's actually been checked, following the same pattern this repo's
[Linux README](../linux/README.md#status) uses to separate "tested" from "should work":

- **Verified on real hardware**: Galaxy S9+ (Android 10, API 29) and Galaxy S21 (Android 15, API
  35) — both Samsung/One UI, but a large version and chip-generation gap. Volume Cap both halves,
  Real-Time Limiter attach/detach/threshold behavior, `dumpsys` session-discovery format handling,
  cold-launch/task-removal service survival, and the standard battery-optimization exemption flow
  are all confirmed working on both.
- **Not independently verified, architecturally expected to work**: everything not covered by an
  OEM-specific quirk above. `AudioDeviceCallback`, `AudioManager` stream-volume control, and
  `DynamicsProcessing` are all public AOSP framework APIs available since API 26–28 regardless of
  manufacturer; the `dumpsys`-parsing hardening above was specifically built to degrade gracefully
  (format-mismatch fallback, capability check) rather than assume a third device would match either
  of the first two.
- **Unverified and OEM-dependent, flagged rather than assumed**: whether the -2 dB threshold clamp
  generalizes beyond Samsung; whether `dumpsys media.audio_flinger`'s format holds up on non-Samsung
  OEM forks (MIUI, ColorOS, etc.) or a much newer/older Android version than the two tested; every
  non-Samsung OEM's battery/autostart-manager guidance in the app (sourced from dontkillmyapp.com,
  explicitly labeled unverified in the UI itself, not claimed as tested).

No non-Samsung device has been available to test against. If one becomes available, the
`dumpsys`-format and `DynamicsProcessing`-availability questions above are the ones most worth
re-checking first.

### What isn't built

- A `.apk` release build / signing config beyond debug-signed builds.
- Live verification on any non-Samsung device (see [Device compatibility](#device-compatibility)).

## Stack

| Component | Choice |
|---|---|
| Language | Kotlin |
| UI | Plain Android Views (`Activity` + XML layouts) — no Compose; the UI is a couple of toggles and status text, not worth the dependency weight of a first-ever Gradle build on a fresh toolchain |
| Build | Gradle 8.9 (downloaded directly, not via the stale `apt` package) + AGP 8.7.3, `compileSdk`/`targetSdk` 34, `minSdk` 26 |
| Volume Cap | `android.media.AudioManager` + `AudioDeviceCallback` |
| Real-Time Limiter | `android.media.audiofx.DynamicsProcessing` (Limiter stage) attached per-session, sessions discovered via `dumpsys media.audio_flinger` (needs `DUMP`) |

## Requirements

- Android 8.0 (API 26) or later.
- For the Real-Time Limiter's actual (non-zero) coverage: the `android.permission.DUMP`
  permission, grantable only via `adb` from a computer — see below. Without it, Volume Cap still
  works; the limiter simply protects nothing and says so in its status text rather than silently
  claiming coverage it doesn't have.

## Permissions

Everything the app declares, and why — no permission here is used for anything beyond what's
described:

| Permission | Why | Runtime prompt? |
|---|---|---|
| `FOREGROUND_SERVICE`, `FOREGROUND_SERVICE_SPECIAL_USE` | Both features need to keep running while the app isn't in the foreground — this is what backs the persistent notification, the same reason the macOS/Linux builds need a continuously-running background component. | No (normal permission) |
| `POST_NOTIFICATIONS` | Required on Android 13+ to show that persistent status notification at all. | Yes, requested on first enable |
| `RECEIVE_BOOT_COMPLETED` | Lets the app restart itself after a reboot if either feature was left enabled — see "Background survival" above. | No (normal permission) |
| `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` | Used only to open the OS's battery-optimization list screen (`Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS`) so *you* can whitelist the app from there — the app never shows the direct "ignore battery optimizations?" system dialog itself. | No (opens a Settings screen instead) |
| `DUMP` | The Real-Time Limiter's only way to discover other apps' audio sessions (`dumpsys media.audio_flinger`). This is a signature-level permission — Android will not let *any* app request it through a normal permission dialog, at all, regardless of what the app declares. The only way to grant it is `adb shell pm grant` from a computer; see [Setup](#setup-for-the-real-time-limiter) below. | **Cannot** be granted via a dialog — `adb` only |

**Is "ignore battery optimization" required?** Not strictly, but it's recommended and the app tells
you your current status. Android's battery optimization (Doze/App Standby, plus Samsung's own
"sleeping apps" management on this test device) can kill background services to save power —
foreground services are more protected than plain background ones, but not exempt. A hearing-safety
app that silently stops protecting you to save a fraction of a percent of battery is a worse
trade-off than most apps asking for this exemption. The app screen shows whether you're currently
exempt and, if not, a button that opens the OS settings screen where you can add it yourself —
it does not use the more aggressive direct-request dialog, since this build isn't going through
Play Store review where that distinction matters most.

**Does it "always run in background"?** Only while you've enabled Volume Cap and/or the Real-Time
Limiter — there's no background behavior at all with both switches off. With either on, yes, by
design: that's what a continuously-active hearing-safety feature requires, the same as the macOS
menu-bar app and the Linux tray daemon in this repo. It's not hidden — the persistent notification
is required by Android for exactly this reason (so a foreground service can never run invisibly).

## Building from source

```
git clone https://github.com/kbssrikar7/headphonesafety.git
cd headphonesafety/android
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

Needs an Android SDK with `platform-tools`, `build-tools;34.0.0`, and `platforms;android-34`
installed (`sdkmanager` from the [command-line
tools](https://developer.android.com/tools/sdkmanager) can install these); point
`android/local.properties` at it with `sdk.dir=/path/to/Android/Sdk` (this file is gitignored,
create it yourself — it's machine-specific).

### Setup for the Real-Time Limiter

The `DUMP` permission the session-discovery mechanism needs cannot be granted through Android's
normal runtime permission dialog — Android requires it be granted from a computer:

```
adb shell pm grant com.headphonesafety.android android.permission.DUMP
```

This is a one-time step per install. Without it, the app's Volume Cap feature works fully; the
Real-Time Limiter's status text will say `DUMP not granted` and protect zero sessions until this
command is run.

## Usage

1. Install the app and launch it — a single screen with two independent feature toggles, mirroring
   the other platforms' menu structure.
2. **Enable Volume Cap**, pick a headroom percentage (an approximation — Android's stream-volume
   API is index-based, not decibel-based, unlike the other three platforms; see
   [`docs/android-port.md`](../docs/android-port.md) for why).
3. **Enable Real-Time Limiter** (experimental), pick a headroom preset. Run the `adb shell pm
   grant` command above first, or the status text will tell you it's protecting nothing. **On
   Samsung/One UI devices tested, this preset currently has no effect** — the vendor DSP
   hard-pins the actual ceiling to ~-2 dB regardless of what's picked here (see above); still worth
   enabling for the fixed peak-limiting protection it does provide, just don't expect the number
   picked to change anything on this hardware.
4. Both features run from a single foreground service (visible as a persistent notification, same
   reason every other platform in this repo needs a persistent background component) and react
   automatically to headphone/Bluetooth connects and disconnects.
5. If the screen shows "Battery optimization: NOT exempt," tap the button below it — recommended so
   the OS doesn't kill the background service to save power. See [Permissions](#permissions) above.

## Testing checklist

- [x] Volume Cap leaves phone-speaker volume untouched when no headphones are connected (verified
      live: 20 volume-up presses, stream volume unchanged).
- [x] Volume Cap actually clamps when headphones/Bluetooth audio are connected (verified live with
      a real Sony WH-CH720N over Bluetooth: 20 volume-up presses stopped at exactly 13/15, matching
      `floor(15 × (100−10)/100)` for the 10% headroom preset).
- [x] Real-Time Limiter discovers and attaches to real third-party app audio sessions (verified
      live against YouTube, cross-referenced by pid → package name).
- [x] Real-Time Limiter's threshold parameter behavior confirmed via read-back, not assumed
      (found hard-pinned to -2 dB on both test devices, documented above).
- [x] `dumpsys media.audio_flinger` session-table parsing handles both real column layouts found
      across the two test devices (verified live, plus JUnit tests built from the real captured
      dumpsys text of both), and correctly filters out a reserved negative session id found live
      on the S21 instead of wasting an attach attempt on it.
- [x] Real-Time Limiter reports "not available on this device" instead of silent per-session
      failures when `DynamicsProcessing` isn't present (capability check via
      `AudioEffect.queryEffects()`) — confirmed the effect *is* available on both test devices, so
      this branch's UI text itself is unverified (no device without the effect to test against).
- [x] Confirmed session-attached effects are genuinely live in the audio path, not just accepting
      parameters — via a `dumpsys media.audio_flinger` per-session effect-chain diff (attaching/
      releasing an `Equalizer` on the test tone's own session made its entry appear/disappear from
      the chain with `Registered=y Enabled=y Suspended=n`). A dB-accurate acoustic measurement
      (not just activation) is still open — the in-app `AudioPlaybackCaptureConfiguration` harness
      built for this sits upstream of session effects on this device (fixed pad, unrelated to the
      actual signal) and a 3.5mm-out → laptop line-in fallback was attempted and abandoned (the
      laptop's input never received signal). See `docs/android-port.md`'s "Measurement harness"
      note for the full writeup.
- [x] All three candidate workarounds for the -2 dB threshold clamp tested and found to fail
      identically (`Limiter.postGain`, MBC per-band `postGain`, PreEq per-band `gain` — each
      silently reset to 0.0 on readback regardless of the value requested). Confirmed: no
      adjustable `DynamicsProcessing` gain/threshold parameter exists on this hardware at all; see
      `docs/android-port.md`'s "Clamp-workaround arms" note.
- [x] Force-stopping the app (`am force-stop`) and cold-relaunching auto-resumes the service with
      no manual toggle needed — verified live (service confirmed absent via `dumpsys activity
      services` after force-stop, confirmed `isForeground=true` again immediately after relaunch).
- [ ] Reboot auto-resume (`BootReceiver`) — confirmed *registered* correctly, not yet confirmed via
      an actual device reboot (not done without asking first; `adb shell` can't simulate the
      protected `BOOT_COMPLETED` broadcast on this device to test it another way).
- [ ] Battery-optimization exemption's actual effect on service survival over real idle/Doze time —
      the detection UI and settings-screen handoff are verified working; whether exemption
      measurably changes survival time hasn't been tested (would need a long unattended idle
      period, not done this session).
- [ ] Any non-Samsung device — no Pixel/AOSP, Xiaomi, Huawei, OnePlus, Oppo, or Vivo phone has been
      available to test against; the app's manufacturer-specific battery guidance for those makers
      is sourced from dontkillmyapp.com and labeled unverified in the UI itself, not tested here.

## License

MIT — see [LICENSE](../LICENSE).
