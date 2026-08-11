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
