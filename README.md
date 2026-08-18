<div align="center">
  <img src="docs/icon.png" alt="Headphone Safety icon" width="128" height="128">

  # Headphone Safety

  Real-time headphone volume/loudness protection, bringing iOS's "Reduce Loud Sounds" behavior
  to the desktop — on every platform it makes sense for.

  ![License](https://img.shields.io/badge/license-MIT-green)
</div>

---

## Why this exists

iOS has had a built-in "Reduce Loud Sounds" feature for years: a real-time limiter that caps audio
peaks before they reach your headphones, independent of the volume slider. Desktop operating
systems generally don't have an equivalent — the difference is real: extended listening at a given
volume is comfortable on a phone, while the same headphones at a similar volume on a laptop can
start to cause ear pain and fatigue within a much shorter session, simply because nothing on the
desktop side is actively limiting the signal.

Every platform implementation in this repo brings the same two features to that platform, using
whatever mechanism is idiomatic there:

- **Volume Cap** — a fast, simple ceiling on the active output device's volume, applied only when
  headphones (wired or Bluetooth) are the active output.
- **Real-Time Limiter** — true peak limiting of the actual audio signal, so a loud transient inside
  content that's already playing at an "allowed" volume gets caught and capped, not just the
  volume level. This is the one that actually matches what iOS does.

## Platforms

| Platform | Status | |
|---|---|---|
| macOS | Shipping | [`macos/`](macos/) — [README](macos/README.md), [Releases](https://github.com/kbssrikar7/headphonesafety/releases) (tags `v*`) |
| Linux | Shipping (Ubuntu 24.04 GNOME tested; `.deb` is Debian/Ubuntu-family only, see [`linux/README.md`](linux/README.md#requirements-ubuntu-or-any-linux) for other distros) | [`linux/`](linux/) — [README](linux/README.md), [Releases](https://github.com/kbssrikar7/headphonesafety/releases) (tags `linux-v*`) |
| Windows | Shipping — both Volume Cap and Real-Time Limiter fully working, verified live with real measured numbers against real Bluetooth headphones (0 dBFS in, -9.9dB out at a 10dB headroom setting). Requires [VB-Audio Virtual Cable](https://vb-audio.com/Cable/) for the Real-Time Limiter — see [`windows/README.md`](windows/README.md) | [`windows/`](windows/) — [README](windows/README.md) |
| Android | Volume Cap + Real-Time Limiter built (`minSdk` 26; live-tested on two Samsung devices spanning Android 10→15, no non-Samsung device tested yet); no unconditional system-wide limiter exists on Android, see [`android/README.md`](android/README.md#device-compatibility) | [`android/`](android/) — [README](android/README.md) |

Each shipping platform is a self-contained subdirectory with its own build, install, and usage
instructions — start with that platform's README. The `docs/*-port.md` guides are from-scratch
architecture research written before implementation, kept even after a platform ships (see
[`docs/ubuntu-port.md`](docs/ubuntu-port.md), which is what `linux/` was actually built from) as
the record of *why* each platform's design looks the way it does:

- [Windows](docs/windows-port.md) — originally planned around a Windows Audio Processing Object,
  with real-world precedent ([Equalizer APO](https://sourceforge.net/projects/equalizerapo/))
  showing it's achievable without a permission prompt at runtime; confirmed correctly built and
  registered end-to-end, but never actually loaded by `audiodg.exe` on the dev machine across two
  driver stacks (see the doc's "Known blocker" section) — kept as parked reference code. What
  actually ships is a WASAPI-loopback + virtual-cable architecture instead (mirroring macOS's own
  BlackHole-based design), verified live with real measured peak-capping numbers against real
  Bluetooth headphones — see the doc's "Approach B: what was actually built and verified" section.
- [Ubuntu / Linux](docs/ubuntu-port.md) — the most favorable of the platforms researched, and the
  one this repo's `linux/` was actually built from: PipeWire's built-in monitor sources need no
  virtual driver and, for a native (non-Flatpak) app, no permission prompt at all.
- [Android](docs/android-port.md) — the most restrictive of the four; Android is adding this
  natively (Sound Dose / Hearing Wellness) with no public API to extend it, and no public API for
  an unconditional system-wide limiter exists at all (three candidate mechanisms researched, two
  are dead ends — see the doc's "RESOLVED" note). What ships in [`android/`](android/) instead: a
  fully-working Volume Cap, and a Real-Time Limiter with real but partial, per-app, `DUMP`-gated
  coverage — the same honest-caveat category as Windows' Developer Options requirement above.

## License

MIT — see [LICENSE](LICENSE).
