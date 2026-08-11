# Headphone Safety (Linux)

A Linux port of [Headphone Safety](https://github.com/kbssrikar7/headphonesafety), a menu-bar app
that protects your hearing during headphone use by bringing iOS's "Reduce Loud Sounds" behavior to
the desktop. See [`docs/ubuntu-port.md`](docs/ubuntu-port.md) for the full architecture research
this build follows.

## Status

Early bootstrap. Implemented so far:

- **Volume Cap** (prototype): polls the default sink via `pactl` every ~350ms, detects
  headphone-classified sinks, and clamps volume to a configurable headroom below max. Verified
  live against this machine's PipeWire 1.0.5 / `pactl 16.1` stack.

Not yet implemented: Real-Time Limiter, tray UI, native `pipewire` crate migration (currently
shells out to `pactl`), packaging. See the build order in `docs/ubuntu-port.md` and the plan this
was bootstrapped from.

## Stack

| Concern | Choice |
|---|---|
| Language | Rust |
| Audio API | `pactl` shell-out (current) → native `pipewire` crate (`Registry`/`global_remove` events), planned |
| Limiter DSP | PipeWire `module-filter-chain` hosting `lsp-plugins`' LV2 limiter, loaded/unloaded at runtime via `pactl load-module`/`unload-module` |
| Tray icon | [`ksni`](https://crates.io/crates/ksni) (pure-Rust StatusNotifierItem) |
| Packaging | `cargo-deb` → `.deb` |

## Headphone detection

Two signals, combined because real hardware doesn't expose them consistently:

- Bluetooth sinks: `device.bus == "bluetooth"` plus `device.form_factor` containing
  `headphone`/`headset`.
- Wired jacks on combo ALSA sinks (confirmed on this laptop's "Speaker + Headphones" device, which
  has **no** `device.form_factor` property at all): `active_port` flips between `[Out] Speaker`
  and `[Out] Headphones` on physical jack insertion — that's the reliable signal here.

## Running

```
cargo build
HPS_HEADROOM_DB=10 ./target/debug/headphonesafety
```

`HPS_HEADROOM_DB` sets the cap in dB below max (macOS presets: 0, 5, 10, 15, 20). Defaults to 10.

## License

MIT, matching the upstream macOS project.
