//! Volume Cap: clamps the active output device's volume to a configurable headroom below max,
//! but only when that device is headphone-like (wired or Bluetooth).
//!
//! Shell-out prototype (`pactl`) per the build order in docs/ubuntu-port.md — this gets replaced
//! by a native `pipewire` crate client (event-driven, no polling) once the plumbing is proven.

use crate::pactl;
use pactl::Result;
use serde_json::Value;

#[derive(Debug)]
pub struct ClampEvent {
    pub sink_name: String,
    pub from_db: f64,
    pub to_db: f64,
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
    pactl::run(&["set-sink-volume", sink_name, &format!("{db:.2}dB")])?;
    Ok(())
}

/// One poll tick: reads `sink_name`'s current volume, and if it's headphone-classified and over
/// the cap, clamps it. Returns `Some(ClampEvent)` when a clamp actually happened, `None`
/// otherwise (not headphones, already under the cap, sink not found, or a transient read/write
/// error worth logging but not fatal to the poll loop).
///
/// Takes an explicit `sink_name` rather than always checking `pactl`'s live default sink: once
/// the Real-Time Limiter is routed, the default sink *is* the virtual routing sink, not the real
/// device — volume needs to be capped on the real device regardless of whether the limiter is
/// currently active, so callers pass the cached real device name (see `tray::AppTray::master_sink`).
pub fn enforce_cap(sink_name: &str, headroom_db: f64) -> Result<Option<ClampEvent>> {
    let sinks = pactl::list_sinks()?;
    let Some(sink) = sinks.iter().find(|s| s["name"] == sink_name) else {
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

    set_volume_db(sink_name, ceiling)?;
    Ok(Some(ClampEvent {
        sink_name: sink_name.to_string(),
        from_db: db,
        to_db: ceiling,
    }))
}
