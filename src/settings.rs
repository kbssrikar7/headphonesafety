/// User-configurable preferences for the Volume Cap feature.
///
/// Headroom presets mirror the macOS app's dB-below-max options. This is deliberately minimal
/// for the shell-out prototype (Volume Cap only) — a persisted config file and the tray-driven
/// UI to edit these arrive with the native `pipewire` crate migration and `ksni` tray (later
/// build-order steps).
#[derive(Debug, Clone, Copy)]
pub struct Settings {
    pub volume_cap_enabled: bool,
    pub headroom_db: f64,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            volume_cap_enabled: true,
            headroom_db: 10.0,
        }
    }
}

impl Settings {
    /// Loads settings from environment variables, falling back to defaults.
    /// `HPS_HEADROOM_DB` accepts one of the macOS app's presets: 0, 5, 10, 15, 20.
    pub fn from_env() -> Self {
        let mut settings = Self::default();
        if let Ok(raw) = std::env::var("HPS_HEADROOM_DB") {
            if let Ok(db) = raw.parse::<f64>() {
                settings.headroom_db = db;
            }
        }
        settings
    }
}
