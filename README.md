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
- **Routing plumbing** (prototype, no DSP yet): `module-virtual-sink` load/switch/revert cycle,
  with every state change verified via readback and reverts retried until they land (see
  `src/routing.rs`). Verified live, both structurally (`pw-dump` graph, sink-input linkage) and
  acoustically (a played test tone was recorded back out of the real device's monitor,
  non-silent). Run `./target/debug/headphonesafety test-routing` to exercise it manually.
- **Real-Time Limiter** (prototype, Zam DSP): `module-ladspa-sink` hosting Zam's `ZaMaximX2`
  true peak limiter/maximizer (see `src/limiter.rs`). Verified live with logged peak dB, not by
  ear: a source tone at -1.1dB was capped to -10.1dB output against a -10dB ceiling. Run
  `./target/debug/headphonesafety test-limiter` to exercise it manually. `lsp-plugins`' LADSPA
  limiter (`lsp-plugins-ladspa.so`, label `.../limiter_stereo`) is the eventual primary DSP per
  the plan, but has ~24 input control ports (several with LADSPA hint-decoding quirks in
  `analyseplugin`'s output) versus Zam's clean 3 — wiring it up is follow-up work.

Not yet implemented: tray UI, native `pipewire` crate migration (currently shells out to
`pactl`), packaging, and swapping in `lsp-plugins` as the primary limiter. See the build order in
`docs/ubuntu-port.md` and the plan this was bootstrapped from.

## Stack

| Concern | Choice |
|---|---|
| Language | Rust |
| Audio API | `pactl` shell-out (current) → native `pipewire` crate (`Registry`/`global_remove` events), planned |
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
limiter, does **not** apply this prefix — the sink is named exactly `sink_name=`. The
`pulse.module.id` lookup handles both without needing to know which.)

## Running

```
cargo build
HPS_HEADROOM_DB=10 ./target/debug/headphonesafety
```

`HPS_HEADROOM_DB` sets the cap in dB below max (macOS presets: 0, 5, 10, 15, 20). Defaults to 10.

## License

MIT, matching the upstream macOS project.
