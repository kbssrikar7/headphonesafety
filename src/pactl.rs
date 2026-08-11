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
