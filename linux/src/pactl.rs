//! Small shared `pactl` shell-out helpers used by both `volume_cap` and `routing`.

use serde_json::Value;
use std::error::Error;
use std::process::Command;

pub type Result<T> = std::result::Result<T, Box<dyn Error>>;

pub fn run(args: &[&str]) -> Result<String> {
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

pub fn run_json(args: &[&str]) -> Result<Value> {
    let raw = run(args)?;
    Ok(serde_json::from_str(&raw)?)
}

pub fn default_sink_name() -> Result<String> {
    let info = run_json(&["-f", "json", "info"])?;
    info["default_sink_name"]
        .as_str()
        .map(str::to_owned)
        .ok_or_else(|| "default_sink_name missing from `pactl info`".into())
}

pub fn list_sinks() -> Result<Vec<Value>> {
    let sinks = run_json(&["-f", "json", "list", "sinks"])?;
    Ok(sinks.as_array().cloned().unwrap_or_default())
}

/// A loaded module: `(id, name, argument)`.
///
/// Deliberately uses `pactl list modules short` (tab-separated text), not `-f json` — the JSON
/// form omits the module's numeric id entirely for `pipewire-pulse` compat modules like
/// `module-ladspa-sink`/`module-virtual-sink` (confirmed live: `properties` has no `object.id`
/// for them, unlike native PipeWire modules). The id only appears in the plain-text `Module #N`
/// header, which `short` format exposes directly as the first tab-separated field.
pub fn list_modules_short() -> Result<Vec<(String, String, String)>> {
    let raw = run(&["list", "modules", "short"])?;
    Ok(raw
        .lines()
        .filter_map(|line| {
            let mut parts = line.splitn(3, '\t');
            Some((
                parts.next()?.to_string(),
                parts.next()?.to_string(),
                parts.next().unwrap_or("").to_string(),
            ))
        })
        .collect())
}
