//! Volume Cap: clamps the active output device's volume to a configurable headroom below max,
//! but only when that device is headphone-like (wired or Bluetooth).
//!
//! Shell-out prototype (`pactl`) per the build order in docs/ubuntu-port.md — this gets replaced
//! by a native `pipewire` crate client (event-driven, no polling) once the plumbing is proven.

use serde_json::Value;
use std::error::Error;
use std::process::Command;

type Result<T> = std::result::Result<T, Box<dyn Error>>;

#[derive(Debug)]
pub struct ClampEvent {
    pub sink_name: String,
    pub from_db: f64,
    pub to_db: f64,
}

fn run_pactl(args: &[&str]) -> Result<String> {
    let output = Command::new("pactl").args(args).output()?;
    if !output.status.success() {
        return Err(format!(
            "pactl {:?} failed: {}",
            args,
            String::from_utf8_lossy(&output.stderr)
        )
        .into());
    }
    Ok(String::from_utf8(output.stdout)?)
}

fn default_sink_name() -> Result<String> {
    let raw = run_pactl(&["-f", "json", "info"])?;
    let info: Value = serde_json::from_str(&raw)?;
    info["default_sink_name"]
        .as_str()
        .map(str::to_owned)
        .ok_or_else(|| "default_sink_name missing from `pactl info`".into())
}

fn list_sinks() -> Result<Vec<Value>> {
    let raw = run_pactl(&["-f", "json", "list", "sinks"])?;
    let sinks: Vec<Value> = serde_json::from_str(&raw)?;
    Ok(sinks)
}

/// Headphone detection, tuned against real hardware rather than assumed from docs:
///
/// - Bluetooth devices reliably expose `device.bus = "bluetooth"` plus a form factor
///   (`headphone`/`headset`) as a distinct sink.
/// - This laptop's built-in audio is a single combo ALSA sink ("Speaker + Headphones") with no
///   `device.form_factor` property at all — jack detection instead flips `active_port` between
///   `[Out] Speaker` and `[Out] Headphones`. That's the reliable signal for wired 3.5mm jacks on
///   hardware shaped like this.
fn is_headphone_sink(sink: &Value) -> bool {
    let props = &sink["properties"];
    let bus = props["device.bus"].as_str().unwrap_or("");
    let form_factor = props["device.form_factor"].as_str().unwrap_or("");
    if bus == "bluetooth" && (form_factor.contains("headphone") || form_factor.contains("headset"))
    {
        return true;
    }
    if form_factor.contains("headphone") || form_factor.contains("headset") {
        return true;
    }
    sink["active_port"]
        .as_str()
        .map(|p| p.to_lowercase().contains("headphone"))
        .unwrap_or(false)
}

/// Loudest channel's current volume in dB, parsed from pactl's `"-3.33 dB"`-style strings.
/// Returns `None` for an unparseable or `-inf dB` (effectively silent) reading.
fn current_db(sink: &Value) -> Option<f64> {
    sink["volume"]
        .as_object()?
        .values()
        .filter_map(|channel| channel["db"].as_str())
        .filter_map(|s| s.trim_end_matches(" dB").parse::<f64>().ok())
        .fold(None, |max, db| Some(max.map_or(db, |m: f64| m.max(db))))
}

fn set_volume_db(sink_name: &str, db: f64) -> Result<()> {
    run_pactl(&["set-sink-volume", sink_name, &format!("{db:.2}dB")])?;
    Ok(())
}

/// One poll tick: reads the default sink, and if it's headphone-classified and over the cap,
/// clamps it. Returns `Some(ClampEvent)` when a clamp actually happened, `None` otherwise
/// (not headphones, already under the cap, or a transient read/write error worth logging but
/// not fatal to the poll loop).
pub fn enforce_cap(headroom_db: f64) -> Result<Option<ClampEvent>> {
    let default_name = default_sink_name()?;
    let sinks = list_sinks()?;
    let Some(sink) = sinks.iter().find(|s| s["name"] == default_name) else {
        return Ok(None);
    };

    if !is_headphone_sink(sink) {
        return Ok(None);
    }

    let Some(db) = current_db(sink) else {
        return Ok(None);
    };

    let ceiling = -headroom_db;
    if db <= ceiling {
        return Ok(None);
    }

    set_volume_db(&default_name, ceiling)?;
    Ok(Some(ClampEvent {
        sink_name: default_name,
        from_db: db,
        to_db: ceiling,
    }))
}
