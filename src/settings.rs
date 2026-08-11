/// User-configurable preferences for Volume Cap and the Real-Time Limiter.
///
/// Headroom presets mirror the macOS app's dB-below-max options. This is deliberately minimal —
/// a persisted config file arrives with the tray UI (build-order step 6), which is also what
/// will actually read/write `limiter_enabled`: the limiter itself (`limiter.rs`) and the removal
/// watcher (`pw_client.rs`) both exist as standalone, live-verified pieces already, but nothing
/// wires this flag to them yet — that wiring is the tray toggle's job.
#[derive(Debug, Clone, Copy)]
pub struct Settings {
    pub volume_cap_enabled: bool,
    #[allow(dead_code)] // wired up by the tray toggle in build-order step 6
    pub limiter_enabled: bool,
    pub headroom_db: f64,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            volume_cap_enabled: true,
            limiter_enabled: false,
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
