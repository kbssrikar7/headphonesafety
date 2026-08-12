# Headphone Safety (Linux)

The Linux implementation of Headphone Safety, a tray app that protects your hearing during
headphone use by bringing iOS's "Reduce Loud Sounds" behavior to the desktop. See
[`docs/ubuntu-port.md`](../docs/ubuntu-port.md) for the full architecture research this build
follows.

> See the [repo root](../README.md) for the other platforms (macOS is also implemented; Windows
> and Android are architecture guides only, not yet built).

## Status

Feature-complete end-to-end daemon, packaged as a `.deb`. All pieces below are wired together and
live-verified as one running application, not standalone prototypes:

- **Volume Cap**: polls the real output device via `pactl` every ~350ms (`src/volume_cap.rs`),
  detects headphone-classified sinks, clamps volume to a configurable headroom below max. Targets
  the real device explicitly rather than "whatever the default sink is," since the default sink
  becomes the virtual routing sink while the limiter is active.
- **Real-Time Limiter**: `module-ladspa-sink` hosting Zam's `ZaMaximX2` true peak limiter
  (`src/limiter.rs`). Verified with logged peak dB, not by ear: a -1.1dB source tone came out at
  -10.1dB against a -10dB ceiling. `lsp-plugins-ladspa`'s `limiter_stereo` is the eventual primary
  DSP (see `../docs/ubuntu-port.md`) but has ~24 input control ports vs. Zam's clean 3 — swapping it
  in is follow-up work.
- **Device-removal watcher**: a native `pipewire` crate registry watcher on its own thread
  (`src/pw_client.rs`), reporting Audio/Sink removals event-driven, no polling. Wired into the
  daemon loop: when the *currently routed* device disappears, `routing::revert_to_fallback`
  switches to another real, available sink (ranked by PipeWire's own `priority.session`, not just
  "any non-Bluetooth sink" — an earlier version of this picked an unplugged HDMI output and
  silently failed to switch) and unloads the now-dangling module.
- **Startup crash recovery**: `routing::cleanup_stray_routes`, run before anything else at daemon
  startup, matching the macOS README's documented guarantee. Empirically necessary: force-killing
  the daemon while routed left a dangling `module-ladspa-sink` with the default sink still pointed
  at it; confirmed the next startup detects and cleans this up automatically.
- **Tray UI**: a StatusNotifierItem via `ksni` (`src/tray.rs`) — device status, live status line,
  Volume Cap toggle, Real-Time Limiter toggle, headroom radio group, Quit. `AppTray` is the app's
  single source of truth for runtime state (see the module doc comment for why no separate
  `Arc<Mutex<_>>` is needed). Confirmed registered correctly with GNOME's `StatusNotifierWatcher`.
- **Packaging**: `cargo-deb` → `.deb`, with explicit `Depends` on `pipewire-pulse`, `wireplumber`,
  `pulseaudio-utils`, and `zam-plugins` (none of these are shared libraries cargo-deb's automatic
  ELF dependency detection can see) plus auto-detected `libpipewire-0.3-0`/`libc6`.

**Verified live**, including the scenario this whole project exists for: a fake headphone
(reversible null-sink, since no physical Bluetooth hardware was available to test with — this is
the one item on the checklist below that's simulated rather than done with real hardware) was set
as the routed device, the limiter enabled via a real tray click (simulated over D-Bus with
`busctl` + `com.canonical.dbusmenu`, exactly the protocol a real click sends), then the device was
yanked out from under it mid-route. The watcher fired, reverted to the correct fallback device, and
the tray status updated to reflect it — no leftover modules, no hang.

The limiter targets whichever output device is actually active *at the moment you enable it*
(re-resolved on each toggle, not cached at process startup — an earlier version cached it once at
startup, which meant enabling the limiter after switching to different headphones mid-session
silently kept protecting the old device; fixed after catching it live with real Bluetooth
headphones connected). Its virtual sink shows up in system volume controls (e.g. GNOME Settings →
Sound) as `Headphone_Safety_Limiter` while active, not the raw `hps_limiter` module name.

Not yet implemented: `lsp-plugins` as the primary limiter DSP (see above), and a persisted
settings file (toggle/headroom state resets on restart). See `../docs/ubuntu-port.md` for the
original architecture research and `src/routing.rs`/`src/limiter.rs` for what actually shipped.

### Testing checklist (from `../docs/ubuntu-port.md`)

- [x] Volume Cap clamps correctly on a headphone-classified sink, leaves speakers untouched.
- [x] Real-Time Limiter is audible and does not silently do nothing.
- [x] Confirmed via logged peak values that a loud/clipping input is actually capped at the
      output, not just passed through quieter.
- [x] Toggling the limiter off (via a real tray click) reverts the default sink cleanly and
      promptly (sub-second).
- [x] Force-killing the process while the limiter is active does not leave the system silently
      stuck — confirmed the *next launch* detects and cleans it up (see "Startup crash recovery").
- [x] Disconnecting headphones while the limiter is active reverts to a safe output device
      automatically without hanging — verified via a reversible simulated disconnect (see above);
      **not yet verified against real Bluetooth hardware**.
- [x] No component of the rollback path performs a live/synchronous query against a device or
      node that might be mid-removal — by construction (the watcher caches names from `global`
      and reacts to bare `global_remove` ids; `pick_fallback_sink` only ever queries *other*,
      still-present sinks).
- [x] No permission prompt appears during normal operation, on a natively-packaged (`.deb`,
      non-Flatpak) install — confirmed throughout development; nothing in this app's operation
      path (pactl, native `pipewire` registry access, D-Bus tray registration) is gated behind a
      permission the user has to grant.

## Stack

| Concern | Choice |
|---|---|
| Language | Rust |
| Audio API | `pactl` shell-out for Volume Cap and routing/limiter control; native `pipewire` crate (`Registry`/`global_remove` events) for device-removal detection, since that's the one thing shell-out polling structurally can't do |
| Limiter DSP | `module-ladspa-sink` (wraps `module-filter-chain` internally) hosting a single LADSPA limiter — Zam's `ZaMaximX2` initially, `lsp-plugins-ladspa`'s `limiter_stereo` planned — loaded/unloaded at runtime via `pactl load-module`/`unload-module` |
| Tray icon | [`ksni`](https://crates.io/crates/ksni) (pure-Rust StatusNotifierItem) |
| Packaging | `cargo-deb` → `.deb` |

## Headphone detection

Two signals, combined because real hardware doesn't expose them consistently:

- Bluetooth sinks: `device.bus == "bluetooth"` plus `device.form_factor` containing
  `headphone`/`headset`.
- Wired jacks on combo ALSA sinks (confirmed on this laptop's "Speaker + Headphones" device, which
  has **no** `device.form_factor` property at all): `active_port` flips between `[Out] Speaker`
  and `[Out] Headphones` on physical jack insertion — that's the reliable signal here.

## Routing (how the passthrough sink actually gets named)

`module-virtual-sink sink_name=X master=Y` does **not** create a sink literally named `X` on this
PipeWire version — it wraps `libpipewire-module-loopback`, and the resulting sink is named
`input.X`. `src/routing.rs` doesn't hardcode that prefix; it looks the sink up by matching its
`pulse.module.id` property back to the module it just loaded, so it stays correct even if the
naming convention changes in a future PipeWire release. (`module-ladspa-sink`, used by the
limiter, does **not** apply this prefix — the sink is named exactly `sink_name`. The
`pulse.module.id` lookup handles both without needing to know which.)

## Building

Besides a Rust toolchain, the `pipewire` crate needs headers/libs and a working `bindgen` at
build time:

```
sudo apt-get install -y libpipewire-0.3-dev pkg-config libclang-dev clang
```

(`libclang-dev`/`clang` are for `bindgen` specifically — without them the build fails with a
`stdbool.h` not found error from inside `libspa-sys`'s build script, not an obviously
pipewire-related message.)

## Running from source

```
cargo build
HPS_HEADROOM_DB=10 ./target/debug/headphonesafety
```

`HPS_HEADROOM_DB` sets the *initial* cap in dB below max (macOS presets: 0, 5, 10, 15, 20);
change it afterward from the tray menu. Runs as a tray-only app — no terminal UI beyond startup
logging — so look for the headphones icon (GNOME needs the `ubuntu-appindicators` extension, or
equivalent, active to render `StatusNotifierItem` tray icons; this ships active by default on
Ubuntu).

Manual verification subcommands for individual pieces (each prints what it's doing and cleans up
after itself): `test-routing`, `test-limiter`, `test-watcher`.

## Packaging

```
cargo install cargo-deb   # one-time
cargo build --release
cargo deb --no-build
sudo dpkg -i target/debian/headphonesafety-linux_*.deb
```

`cargo deb`'s `Depends` covers everything needed at runtime (see `[package.metadata.deb]` in
`Cargo.toml`) — a machine with just the `.deb` installed doesn't need the build-time
`libclang-dev`/`clang`/`libpipewire-0.3-dev` packages above, those are compile-time only.

The package also installs `/usr/share/applications/headphonesafety.desktop`, so it shows up as a
regular launchable app (e.g. in GNOME's Activities search) — but installing the `.deb` does
**not** make it start automatically at login by itself; that's opt-in (see below), matching how
the macOS build treats "Launch at login" as a documented optional step rather than automatic.

### Launch at login (optional)

```
mkdir -p ~/.config/autostart
cp /usr/share/applications/headphonesafety.desktop ~/.config/autostart/
```

To undo:

```
rm ~/.config/autostart/headphonesafety.desktop
```

## License

MIT, matching the upstream macOS project.
