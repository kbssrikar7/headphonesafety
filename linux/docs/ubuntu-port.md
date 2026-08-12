# Headphone Safety for Ubuntu / Linux — Implementation Guide

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

This document is a from-scratch build guide for the Linux/Ubuntu equivalent, written after
building the macOS version end-to-end and researching the Linux-specific audio stack needed. It
assumes the reader (a future Claude session, or a developer) has **no memory of the macOS build**
— so it explains the *why* behind each design decision, not just the *what*.

**Of the three platforms researched (macOS, Windows, Linux), Linux is the most favorable one for
this project** — it's the only platform where the exact architecture used on macOS (capture the
mixed system output, process it, re-render to real hardware) can be built with **zero permission
prompt of any kind**, confirmed via real-world precedent, and it may need the least custom code
because existing open-source limiter DSP plugins can potentially be reused directly instead of
writing the DSP from scratch.

**Read the "Hard-won lessons" section before writing any rollback/device-switching logic.** Every
one of those lessons cost real debugging time on macOS and the underlying failure modes (timing
windows, misleading success signals, blocking calls on a hot path) are generic enough to plausibly
recur here in some form, even though the concrete APIs are completely different.

---

## Which audio server: assume PipeWire

Modern Ubuntu (22.10+) ships **PipeWire** as the default audio server, with a PulseAudio-
compatible shim layer for backward compatibility. Target PipeWire directly, but design so the
PulseAudio-compatible fallback (`pactl`) still works, since some systems may still be on classic
PulseAudio. This guide covers both where they differ.

## Architecture overview

### Feature 1: Volume Cap

Two viable ways to build this, roughly matching the "shell out" vs "native library" tradeoff:

- **Simplest**: shell out to `wpctl` (WirePlumber control, the modern PipeWire-native tool) —
  `wpctl get-volume @DEFAULT_AUDIO_SINK@` / `wpctl set-volume @DEFAULT_AUDIO_SINK@ <value>`. Or
  the PulseAudio-compatible `pactl get-sink-volume` / `pactl set-sink-volume`. This is quick to
  prototype and matches the level of directness the macOS build used for some things, but
  shelling out has real downsides for a background daemon: parsing text output is fragile, and
  polling by spawning a process every ~400ms is wasteful and adds latency.
- **Better for a real daemon**: link against `libpipewire` (or `libpulse` if targeting classic
  PulseAudio) directly and use its native volume APIs — get proper event-driven updates instead of
  polling, and avoid subprocess overhead entirely. Recommend building the *first* working version
  with `wpctl`/`pactl` shell-outs (fast to verify the concept live, exactly like early iteration on
  macOS did with `defaults write` + reading `system_profiler` output), then migrating to a native
  `libpipewire` client once the behavior is proven.

**dB math**: PipeWire/PulseAudio volume is normally represented as a linear scalar (0-65536
internally, or 0-100%+ in most tool output, since Pulse allows >100% for extra gain). Converting
this to/from dB for the headroom-cap concept needs the same kind of scalar↔dB translation the
macOS build did (`20 * log10(scalar)` is the standard formula, matching how PulseAudio itself
defines its dB scale — verify against `pa_sw_volume_to_dB` / `pa_sw_volume_from_dB` in
`libpulse`'s `volume.h` if linking natively, rather than assuming the formula is exactly right on
sight).

**Detecting "is this a headphone-like device"**: sink properties. Run `pactl list sinks` (or the
native introspection API's `pa_sink_info`) and check `device.form_factor` (values like
`headphone`, `headset`) and `device.bus` (`bluetooth` for Bluetooth devices) properties, similar
in spirit to macOS's `kAudioDevicePropertyTransportType` check.

Poll loop: same shape as macOS, ~300-400ms, read current volume, clamp if over the cap. No
permission needed for any of this on either PulseAudio or PipeWire.

### Feature 2: Real-Time Limiter

**No virtual-device driver install is needed at all** — unlike macOS (which needed BlackHole) and
the Windows fallback approach (which needs a virtual cable), PipeWire/PulseAudio's loopback
capability is built into the audio server itself.

**Capturing system audio**: every sink automatically exposes a `<sink_name>.monitor` source that
represents exactly what's being sent to that sink — this is the direct equivalent of what
BlackHole provided on macOS, except it requires no extra software at all. Confirmed via real-world
precedent (see "Permission model" below) that a normal, natively-installed (non-Flatpak)
application can record from a monitor source with **no permission prompt whatsoever.**

**Recommended approach — PipeWire `filter-chain` module**: PipeWire ships a `filter-chain` module
that can host LADSPA/LV2 audio plugins directly inside the graph, wired between a source and a
sink, entirely via **configuration** (a `.conf` file), no custom C/C++ required for the DSP part
itself. Concretely:

1. Look for an existing, well-tested compressor/limiter LADSPA or LV2 plugin already packaged for
   Ubuntu — candidates to check first: **Calf Studio Gear**'s limiter/compressor (`calf-plugins`
   package), or classic `swh-plugins` (contains various dynamics-processing LADSPA plugins,
   including limiters). `apt search` for these on the target system and inspect what's available.
2. Configure a `filter-chain` node in a PipeWire config snippet (e.g.
   `~/.config/pipewire/pipewire.conf.d/headphone-safety-limiter.conf`) that:
   - Creates a virtual/null sink to act as the routing point (PipeWire's equivalent of BlackHole,
     but built-in — no separate driver needed, just a config-defined null sink node)
   - Loads the chosen limiter plugin, with its threshold/ceiling parameter tied to the same
     dB-headroom-preset concept as Volume Cap
   - Routes: null sink (system audio target) → filter-chain (limiter) → real output device
3. Have the app itself just manage: (a) writing/removing this PipeWire config snippet, (b)
   reloading PipeWire or restarting the relevant module to apply it, and (c) switching the
   *default* sink to the null/virtual sink when the feature is enabled, back to the real device
   when disabled.

This is a meaningfully smaller build than either macOS or Windows, since the actual peak-limiting
DSP is an existing, already-correct plugin rather than something written and debugged from
scratch.

**Fallback approach — custom PipeWire filter client**: if a suitable existing plugin can't be
found or configured cleanly, write a small native client using `libpipewire`'s **Filter API**
(`pw_filter_new`, process callback) that does the limiting inline in C. Use the same peak-limiter
algorithm sketch as the Windows guide (envelope follower with fast attack / slower release,
computing a gain-reduction factor against a ceiling) — it's DSP, not platform-specific, so the
algorithm itself transfers directly between the Windows and Linux ports. This runs as a normal
userspace process talking to PipeWire's socket, same permission characteristics as the
`filter-chain`-module approach (see below) since it's a native, non-sandboxed client either way.

**Setting the default sink**: `pactl set-default-sink <name>` or the native
`pa_context_set_default_sink` — this is a fully public, documented, unprivileged operation on both
PulseAudio and PipeWire's compatibility layer. No undocumented API needed, unlike Windows.

### Permission model — confirmed favorable

Researched specifically because it's the whole point of considering this port: whether a native
Linux app doing this kind of system-audio capture/processing needs any user consent.

- **Classic PulseAudio**: no permission concept for this at all — has never gated monitor-source
  access.
- **PipeWire**: integrates with `xdg-desktop-portal` for audio/video capture, but this portal
  system is specifically a **Flatpak sandboxing mechanism**. Confirmed via real-world precedent:
  **OBS Studio**'s natively-packaged build (`.deb`/PPA, not the Flatpak build) has full,
  unprompted access to PipeWire audio capture by default — the portal/consent-dialog flow only
  applies to the sandboxed Flatpak build, which needs explicit `flatpak override` grants or
  Flatseal specifically *because* of that sandboxing. A plain native binary, installed normally
  (not via Flatpak), talking to PipeWire's socket directly, does not go through the portal at all.

**Practical implication**: distribute this as a native package (`.deb`, or a plain binary/AppImage
that isn't Flatpak-sandboxed) — not as a Flatpak — to keep the permission-free property. If it's
ever packaged as a Flatpak for wider distribution (e.g. Flathub), expect to need the portal-based
audio-capture permission flow, the same tradeoff Windows/macOS live with.

*(One nuance encountered during research, likely not relevant here but worth knowing: PipeWire has
a newer "per-application audio capture" API — grabbing one specific running app's individual
stream, not the whole mixed sink output — that reportedly can trigger a one-time portal prompt
even for native apps. This project only needs the classic "capture the mixed sink output" style,
which is the unrestricted, PulseAudio-compatible path — don't accidentally reach for the
per-application capture API instead.)*

---

## Hard-won lessons from the macOS build (read before writing rollback logic)

The macOS build went through multiple live-tested rounds where a seemingly-safe revert mechanism
turned out not to be safe in practice. These are the actual root causes found — treat all of them
as *generically likely to recur* on Linux, not macOS-specific quirks, even though PipeWire's API
shape is quite different from CoreAudio's:

1. **A "successful" API call is not proof the change took effect.** On macOS,
   `AudioObjectSetPropertyData` for the default output device silently returns success even when
   targeting a device that no longer exists — it just no-ops. Any Linux equivalent (`pactl
   set-default-sink`, or the native `pa_context_set_default_sink` callback) should be **verified
   by reading the actual resulting default sink back**, not trusted from a success callback/exit
   code alone.

2. **There is a real timing window right after a device disconnects where operations can
   transiently fail/no-op, even targeting a device that's still valid.** On macOS, a revert
   attempt to a guaranteed-present device (built-in speakers) failed silently in the split-second
   right after a Bluetooth disconnect event, then succeeded a moment later when retried. Design
   any revert/rollback logic here to **retry until it actually lands** (checked via readback, per
   #1), not attempt once and move on.

3. **Per-device property queries against a device mid-disconnect can block the calling thread
   indefinitely.** This was the most dangerous macOS failure mode: a plain, read-only property
   query against a device that was mid-Bluetooth-teardown froze the app's entire main thread —
   which also froze the very poll loop that was supposed to detect and fix the problem. **Design
   rule coming out of this**: cache whatever sink/device identifiers are needed for rollback
   *once, at session start, while everything is healthy* — avoid re-enumerating or re-querying
   live device state from inside a recovery/rollback code path. On PipeWire, be specifically
   cautious around any synchronous call that touches a node/device that might be mid-removal;
   prefer reacting to `pw_registry` removal events (which tell you a node/device disappeared)
   over polling and querying to find out.

4. **A device-list-change listener should react unconditionally, not re-verify by querying.** The
   first (buggy) macOS fix tried to have the disconnect-listener itself check "is my device still
   present" before reacting — but that check was itself the kind of query that could hang. The
   working fix: treat any relevant device-list-change event while the limiter is running as
   "assume something's wrong, revert" unconditionally, without an extra verification query in the
   hot path. A false-positive revert (e.g. from an unrelated device being plugged in) is cheap —
   the limiter just restarts on the next tick if the real device is still there. A hang is not
   recoverable. Bias toward the cheap failure mode. On PipeWire, this maps to reacting directly to
   `pw_registry_events.global_remove` for the tracked device, not re-querying the registry to
   confirm.

5. **Test the actual failure scenario live, not just the happy path.** Every fix above was found
   by actually disconnecting real Bluetooth headphones while the limiter was running and watching
   what happened — not by reasoning about API docs alone (the documented behavior did not predict
   several of these failure modes). Budget real time for the same kind of testing here: toggle
   on/off, force-kill the process while the limiter is active, and physically disconnect Bluetooth
   headphones mid-playback, each followed by confirming audio is actually restored (not just that
   no error was logged).

6. **Independent, out-of-process safety net during development.** While iterating and testing
   live, it was valuable to have a *separate* watchdog process (not part of the app under test)
   that would force-revert the default sink after a timeout regardless of what the app itself was
   doing — this caught cases where the app's own recovery logic was still broken, without leaving
   the developer stuck with no sound for an extended period during testing. Recommend the same
   here: a small standalone script (`pactl set-default-sink <real-device>` on a delay, run
   separately from the app under test) armed before each live test.

---

## Recommended build order

Mirrors how the macOS version was actually built and debugged:

1. **Volume Cap only**, via `wpctl`/`pactl` shell-outs first, then migrate to native `libpipewire`
   calls once proven. No permission concerns, low risk, and it's the fallback behavior for
   everything else.
2. **Null sink + pass-through filter-chain, no DSP.** Get a null sink created, default output
   switched to it, and a filter-chain routing audio through unmodified to the real device — and
   *confirm it's audible* before adding any processing. (On macOS, several of the hardest bugs
   were in getting audio to flow through the pipeline at all — the actual limiting logic was
   comparatively simple once the plumbing worked. Expect the same shape of difficulty here, even
   though the specific bugs will be different.)
3. **Insert the limiter plugin** into the filter-chain (or add the custom DSP if using the
   fallback native-client approach), verify audibly that it's a true peak limiter — test with a
   deliberately loud/clipping test signal, confirm the *output* level is actually capped (log
   peak values before/after, don't just judge "it sounds a bit quieter" by ear).
4. **Rollback and safety testing**, following the lessons above: toggle-off, crash recovery,
   device-disconnect recovery — each verified live against real hardware, each with a documented
   test showing it actually works, not just "should work" per the API docs.
5. **UI**: a system tray icon. Modern GNOME doesn't support the classic `XEmbed` tray protocol; use
   `libayatana-appindicator` (or its GTK bindings) for a `StatusNotifierItem`-based tray icon that
   works across GNOME/KDE/etc., with a menu mirroring the macOS app's structure — device status,
   cap toggle + headroom presets, limiter toggle + status line.
6. **Packaging**: a `.deb` (via `debhelper`/`dpkg-deb`) or an AppImage — avoid Flatpak specifically
   to preserve the permission-free property established above, unless a later revision explicitly
   wants to accept the Flatpak portal tradeoff for wider distribution reach.

---

## Testing checklist (same bar as the macOS version)

- [ ] Volume Cap clamps correctly on a headphone-classified sink, leaves speakers untouched.
- [ ] Real-Time Limiter is audible and does not silently do nothing.
- [ ] Confirmed via logged peak values that a deliberately loud/clipping input signal is actually
      capped at the output, not just passed through at a slightly lower level.
- [ ] Toggling the limiter off reverts the default sink cleanly and promptly.
- [ ] Force-killing the process while the limiter is active does not leave the system silently
      stuck on the null sink — either a startup check reverts it on next launch, or (better) it
      recovers automatically some other way.
- [ ] Physically disconnecting headphones while the limiter is active reverts to a safe output
      device automatically, without the app hanging.
- [ ] No component of the rollback path performs a live/synchronous query against a device or node
      that might be mid-removal.
- [ ] Confirmed no permission prompt appears during normal operation, on a natively-packaged
      (non-Flatpak) install.

## Key references

- [PipeWire documentation](https://docs.pipewire.org/) — start with the Access Control page
  (`page_access.html`) and the filter-chain module docs
- [PipeWire GitHub (source + examples)](https://github.com/PipeWire/pipewire) — look at
  `src/modules/module-filter-chain.c` and any example filter-chain `.conf` files in the repo for
  real, working syntax rather than guessing the config format
- [WirePlumber documentation](https://pipewire.pages.freedesktop.org/wireplumber/) — the session
  manager `wpctl` talks to; relevant for default-device switching and policy
- [obs-pipewire-audio-capture (GitHub)](https://github.com/dimtpap/obs-pipewire-audio-capture) —
  real-world example of a native app doing exactly this class of PipeWire audio capture; useful
  as a working reference implementation, and its issue tracker/docs are a good source of practical
  gotchas
- Candidate limiter plugins to check availability of on the target system: `calf-plugins` (Calf
  Studio Gear), `swh-plugins` (classic LADSPA dynamics plugins) — `apt search calf` / `apt search
  swh-plugins`
- `libayatana-appindicator` — for the system tray icon, since classic XEmbed trays are unsupported
  on modern GNOME

## Why this is the most favorable of the three platforms (context, in case it comes up)

For context if this is ever compared side-by-side with the macOS and Windows versions: macOS's
driver mechanism (`AudioServerPlugIn`) is structurally forbidden from forwarding processed audio
to a different physical device — confirmed in Apple's own header docs — so *any* macOS
architecture for this feature necessarily involves a separate userspace process doing
capture-style reads, which is exactly what triggers the Microphone/Screen Recording permission,
unavoidably. Windows can avoid its permission prompt too, but only via the heavier Audio
Processing Object route (real-time-safe COM code, registry-based unsigned-driver workaround,
admin-rights install). Linux gets the same "no permission prompt" outcome as Windows's APO route,
but via what is comparatively the *simplest* mechanism of the three — a built-in, first-class
monitor-source capture facility that ships in the audio server itself, confirmed unprivileged for
native apps via real-world precedent (OBS Studio), with no virtual-driver install and no
custom-DSP requirement if an existing LADSPA/LV2 limiter plugin can be wired in via configuration.
