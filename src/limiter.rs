//! Real-Time Limiter: inserts a true peak limiter between the routing point and the real output
//! device via `module-ladspa-sink` (build-order step 4 in docs/ubuntu-port.md).
//!
//! Uses the Zam plugins' `ZaMaximX2` (LADSPA, already installed on this machine) as the initial
//! limiter, per the plan's "Zam first for a fast smoke test" — `lsp-plugins`' LADSPA-flavored
//! `Limiter Stereo` (`lsp-plugins-ladspa.so`, label
//! `http://lsp-plug.in/plugins/ladspa/limiter_stereo`) is the eventual primary choice but has
//! ~24 input control ports (several with LADSPA hint-decoding quirks in `analyseplugin`'s output)
//! versus Zam's 3 — wiring it up correctly is follow-up work, not done here.
//!
//! `ZaMaximX2`'s input control ports, confirmed via `analyseplugin` (order matters — `pactl`'s
//! `control=` argument is positional, not named):
//!   1. Release          (1 to 100, logarithmic, ms-like release time)
//!   2. Output Ceiling   (-30 to 0 dB) — mapped to `-headroom_db`
//!   3. Threshold        (-30 to 0 dB) — set equal to the ceiling for hard/brick-wall limiting

use crate::pactl;
use crate::routing::Route;
use pactl::Result;

const PLUGIN: &str = "ZaMaximX2-ladspa";
const LABEL: &str = "ZaMaximX2";
const DEFAULT_RELEASE: f64 = 3.16228;

/// Loads the limiter over `master_sink`, capping output peaks at `headroom_db` below max.
/// Returns a `routing::Route` — same shape as the plumbing-only passthrough, so
/// `routing::set_default`/`routing::revert` work unchanged for switching to/from and tearing
/// down the limiter.
pub fn load(master_sink: &str, headroom_db: f64, sink_name_hint: &str) -> Result<Route> {
    let ceiling = -headroom_db;
    let module_id = pactl::run(&[
        "load-module",
        "module-ladspa-sink",
        &format!("sink_name={sink_name_hint}"),
        &format!("master={master_sink}"),
        &format!("plugin={PLUGIN}"),
        &format!("label={LABEL}"),
        &format!("control={DEFAULT_RELEASE},{ceiling:.2},{ceiling:.2}"),
    ])?
    .trim()
    .to_string();

    let sinks = pactl::list_sinks()?;
    let sink_name = sinks
        .iter()
        .find(|s| s["properties"]["pulse.module.id"] == module_id)
        .and_then(|s| s["name"].as_str())
        .map(str::to_owned)
        .ok_or_else(|| {
            format!("loaded module-ladspa-sink {module_id} but couldn't find its sink by pulse.module.id")
        })?;

    Ok(Route {
        module_id,
        sink_name,
        master_sink: master_sink.to_string(),
    })
}
