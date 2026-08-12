//! Routing: creates a virtual "passthrough" sink in front of the real output device, so later
//! work (the Real-Time Limiter) can insert DSP between them without restarting PipeWire.
//!
//! No DSP here yet — this is deliberately just the plumbing (build-order step 3 in
//! docs/ubuntu-port.md): prove a routing-point sink can be created, made default, and torn down
//! cleanly, before any limiter plugin is added (step 4).
//!
//! Applies the macOS build's hard-won rollback lessons (see docs/ubuntu-port.md):
//! - A "successful" pactl call is not proof the change landed — every switch is verified by
//!   reading the state back, not trusted from an exit code alone.
//! - Reverts retry until the readback confirms they landed, rather than firing once.

use crate::pactl;
use pactl::Result;
use serde_json::Value;
use std::thread;
use std::time::Duration;

/// A live passthrough route: a virtual sink (`sink_name`) forwarding to `master_sink`, backed by
/// a loaded `module-virtual-sink` instance (`module_id`).
#[derive(Debug)]
pub struct Route {
    pub module_id: String,
    pub sink_name: String,
    pub master_sink: String,
}

const READBACK_RETRIES: u32 = 5;
const READBACK_RETRY_DELAY: Duration = Duration::from_millis(100);

/// Loads `module-virtual-sink` over `master_sink` and resolves the resulting sink's actual pactl
/// name.
///
/// The resolved name is **not** simply `sink_name_hint`: on this PipeWire version,
/// `module-virtual-sink` (which wraps `libpipewire-module-loopback`) names the resulting sink
/// `input.<sink_name_hint>` — confirmed by loading it live and inspecting `pactl -f json list
/// sinks`. Rather than hardcode that prefix (which could change across PipeWire versions), the
/// sink is looked up by its `pulse.module.id` property, which reliably ties it back to the module
/// this function just loaded regardless of naming.
pub fn load_passthrough(master_sink: &str, sink_name_hint: &str) -> Result<Route> {
    let module_id = pactl::run(&[
        "load-module",
        "module-virtual-sink",
        &format!("sink_name={sink_name_hint}"),
        &format!("master={master_sink}"),
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
            format!("loaded module-virtual-sink {module_id} but couldn't find its sink by pulse.module.id")
        })?;

    Ok(Route {
        module_id,
        sink_name,
        master_sink: master_sink.to_string(),
    })
}

/// Sets the default sink and retries until a readback confirms it actually took effect —
/// hard-won lesson #1/#2: a successful exit code is not proof, and the first attempt can
/// transiently no-op.
pub fn set_default(sink_name: &str) -> Result<()> {
    for attempt in 0..READBACK_RETRIES {
        pactl::run(&["set-default-sink", sink_name])?;
        if pactl::default_sink_name()? == sink_name {
            return Ok(());
        }
        if attempt + 1 < READBACK_RETRIES {
            thread::sleep(READBACK_RETRY_DELAY);
        }
    }
    Err(format!(
        "set-default-sink {sink_name} did not take effect after {READBACK_RETRIES} attempts (readback never matched)"
    )
    .into())
}

/// Switches the default sink back to the real device (retrying until readback confirms it) and
/// unloads the passthrough module.
pub fn revert(route: &Route) -> Result<()> {
    set_default(&route.master_sink)?;
    pactl::run(&["unload-module", &route.module_id])?;
    Ok(())
}

/// Emergency revert for when `route.master_sink` itself has vanished (e.g. the headphones were
/// unplugged/disconnected while routed through it) — switching *back to* the vanished device is
/// impossible, so this picks another available real sink instead.
///
/// Tolerates the module already being gone: PipeWire may have already torn it down on its own
/// once its `master=` device disappeared, so an `unload-module` failure here is logged but not
/// treated as fatal — the goal is "audio is flowing through a real device again," which the
/// `set_default` above already accomplished by the time this is reached.
pub fn revert_to_fallback(route: &Route) -> Result<String> {
    let fallback = pick_fallback_sink(&route.sink_name)?;
    set_default(&fallback)?;
    if let Err(e) = pactl::run(&["unload-module", &route.module_id]) {
        eprintln!("note: unload-module {} failed (likely already gone since its master vanished): {e}", route.module_id);
    }
    Ok(fallback)
}

/// Startup-only: cleans up any stray routing/limiter modules left behind by an unclean shutdown
/// (crash, force-kill) — matching the macOS build's documented guarantee ("a startup check
/// reverts stray routing back to a real device the next time it launches"). Empirically confirmed
/// necessary during development: `kill`-ing this app while the limiter was active left a dangling
/// `module-ladspa-sink` behind with the default sink still pointed at it.
pub fn cleanup_stray_routes() -> Result<()> {
    for (module_id, name, argument) in pactl::list_modules_short()? {
        if name != "module-virtual-sink" && name != "module-ladspa-sink" {
            continue;
        }
        if !argument.contains("sink_name=hps_") {
            continue;
        }

        println!("cleaning up stray routing module {module_id} ({name}: {argument}) left by a previous unclean shutdown");

        // If this stray sink is currently the default, get off it *before* unloading it, so the
        // machine isn't left silently stuck on a sink that's about to disappear.
        if let Ok(current_default) = pactl::default_sink_name() {
            let sinks = pactl::list_sinks().unwrap_or_default();
            let is_default = sinks.iter().any(|s| {
                s["name"] == current_default && s["properties"]["pulse.module.id"] == module_id
            });
            if is_default {
                if let Ok(fallback) = pick_fallback_sink(&current_default) {
                    let _ = set_default(&fallback);
                }
            }
        }

        if let Err(e) = pactl::run(&["unload-module", &module_id]) {
            eprintln!("failed to unload stray module {module_id}: {e}");
        }
    }
    Ok(())
}

/// Picks a real (non-virtual), currently-present sink to fall back to, excluding `exclude` (the
/// routing sink whose master just vanished). Prefers a non-Bluetooth device, since Bluetooth
/// devices are exactly the kind that can also vanish mid-session; among those, prefers the
/// highest `priority.session` (WirePlumber's own ranking of "which sink should be default").
///
/// The priority sort matters, not just as a tie-breaker: confirmed live that picking a
/// technically-present but *unplugged* sink (an HDMI output with nothing connected to it, which
/// still shows up in `pactl list sinks`) makes `set-default-sink` to it silently never take
/// effect — `priority.session` is exactly the signal that separates "present" from "actually
/// usable," since PipeWire/WirePlumber rank a laptop's real built-in output far above an
/// unplugged HDMI port (1000 vs. ~500-700, observed live on this machine).
fn pick_fallback_sink(exclude: &str) -> Result<String> {
    let sinks = pactl::list_sinks()?;
    let mut candidates: Vec<&Value> = sinks
        .iter()
        .filter(|s| s["name"] != exclude)
        .filter(|s| s["properties"]["node.virtual"] != "true")
        .collect();

    let priority = |s: &Value| -> i64 {
        s["properties"]["priority.session"]
            .as_str()
            .and_then(|p| p.parse().ok())
            .unwrap_or(0)
    };
    candidates.sort_by_key(|s| std::cmp::Reverse(priority(s)));

    candidates
        .iter()
        .find(|s| s["properties"]["device.bus"] != "bluetooth")
        .or_else(|| candidates.first())
        .and_then(|s| s["name"].as_str())
        .map(str::to_owned)
        .ok_or_else(|| "no fallback sink available (all sinks vanished or virtual)".into())
}
