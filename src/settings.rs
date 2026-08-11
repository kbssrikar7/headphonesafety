/// Startup preferences for Volume Cap and the Real-Time Limiter.
///
/// Headroom presets mirror the macOS app's dB-below-max options. Only the *initial* values live
/// here — at runtime, `tray::AppTray` is the single source of truth for toggle state (see its
/// doc comment for why), read and mutated through its `ksni::Handle`. A persisted config file
/// (so preferences survive a restart) is follow-up work.
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
