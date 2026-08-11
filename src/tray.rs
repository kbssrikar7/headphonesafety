//! Tray UI (build-order step 6): a StatusNotifierItem tray icon via `ksni`, mirroring the macOS
//! app's menu structure — device status, Volume Cap toggle + headroom presets, Real-Time Limiter
//! toggle + status, Quit.
//!
//! `AppTray` *is* the app's shared mutable state (not a separate `Arc<Mutex<_>>`): ksni's
//! `Handle::update(|tray| ...)` already gives synchronized, blocking access to it from any
//! thread, running the closure on ksni's own internal service thread. Menu clicks mutate `self`
//! directly (running on that same thread); the main poll loop reaches in via `handle.update(...)`
//! for Volume Cap status and the device-removal watcher's auto-revert.

use crate::{limiter, pactl, routing};
use ksni::menu::{CheckmarkItem, RadioGroup, RadioItem, StandardItem};
use ksni::MenuItem;

pub const HEADROOM_PRESETS: [f64; 5] = [0.0, 5.0, 10.0, 15.0, 20.0];

pub struct AppTray {
    pub volume_cap_enabled: bool,
    pub limiter_enabled: bool,
    pub headroom_db: f64,
    /// The real output device the limiter targets. Re-resolved to the *current* default sink
    /// every time the limiter is turned on (see `toggle_limiter`) — not cached once at process
    /// startup. An earlier version cached this only at startup, which meant enabling the limiter
    /// after switching to a different output device (e.g. connecting Bluetooth headphones after
    /// the app had already started against the laptop speakers) silently kept protecting the old
    /// device instead of the one actually in use — confirmed live as a real bug, not a
    /// hypothetical. Once a route is active, this is that route's `master_sink` and stays fixed
    /// for the lifetime of that route (hard-won lesson #3 still applies *within* one route: don't
    /// re-resolve the identifier you're actively routing through).
    pub master_sink: String,
    /// Present while the Real-Time Limiter is actively routed through a virtual sink.
    pub limiter_route: Option<routing::Route>,
    pub status: String,
}

impl AppTray {
    pub fn new(master_sink: String, headroom_db: f64, volume_cap_enabled: bool) -> Self {
        Self {
            volume_cap_enabled,
            limiter_enabled: false,
            headroom_db,
            master_sink,
            limiter_route: None,
            status: "idle".into(),
        }
    }

    fn toggle_limiter(&mut self) {
        if let Some(route) = self.limiter_route.take() {
            match routing::revert(&route) {
                Ok(()) => self.status = "Real-Time Limiter off".into(),
                Err(e) => self.status = format!("failed to revert limiter: {e}"),
            }
            self.limiter_enabled = false;
            return;
        }

        let current_default = match pactl::default_sink_name() {
            Ok(name) => name,
            Err(e) => {
                self.status = format!("failed to read current output device: {e}");
                return;
            }
        };
        self.master_sink = current_default;

        match limiter::load(&self.master_sink, self.headroom_db, "hps_limiter") {
            Ok(route) => match routing::set_default(&route.sink_name) {
                Ok(()) => {
                    self.status = format!(
                        "Real-Time Limiter on ({:.0} dB headroom, protecting {})",
                        self.headroom_db, route.master_sink
                    );
                    self.limiter_route = Some(route);
                    self.limiter_enabled = true;
                }
                Err(e) => {
                    self.status = format!("failed to switch to limiter: {e}");
                    let _ = pactl::run(&["unload-module", &route.module_id]);
                }
            },
            Err(e) => self.status = format!("failed to load limiter: {e}"),
        }
    }

    fn headroom_index(&self) -> usize {
        HEADROOM_PRESETS
            .iter()
            .position(|&db| (db - self.headroom_db).abs() < 0.01)
            .unwrap_or(2)
    }
}

impl ksni::Tray for AppTray {
    fn id(&self) -> String {
        "headphonesafety".into()
    }

    fn icon_name(&self) -> String {
        "audio-headphones".into()
    }

    fn title(&self) -> String {
        if self.limiter_route.is_some() {
            "Headphone Safety (Limiter On)".into()
        } else {
            "Headphone Safety".into()
        }
    }

    fn menu(&self) -> Vec<MenuItem<Self>> {
        // Live, not cached: shows what's actually plugged in *right now* if the limiter is off
        // (so this doesn't repeat the bug where a stale device name was displayed/targeted), or
        // the device the active route is actually protecting if it's on.
        let device_label = match &self.limiter_route {
            Some(route) => format!("Protecting: {}", route.master_sink),
            None => match pactl::default_sink_name() {
                Ok(name) => format!("Current output: {name}"),
                Err(_) => "Current output: unknown".into(),
            },
        };

        vec![
            StandardItem {
                label: device_label,
                enabled: false,
                ..Default::default()
            }
            .into(),
            StandardItem {
                label: self.status.clone(),
                enabled: false,
                ..Default::default()
            }
            .into(),
            MenuItem::Separator,
            CheckmarkItem {
                label: "Enable Volume Cap".into(),
                checked: self.volume_cap_enabled,
                activate: Box::new(|this: &mut Self| {
                    this.volume_cap_enabled = !this.volume_cap_enabled;
                }),
                ..Default::default()
            }
            .into(),
            CheckmarkItem {
                label: "Enable Real-Time Limiter".into(),
                checked: self.limiter_enabled,
                activate: Box::new(|this: &mut Self| this.toggle_limiter()),
                ..Default::default()
            }
            .into(),
            MenuItem::Separator,
            RadioGroup {
                selected: self.headroom_index(),
                select: Box::new(|this: &mut Self, index| {
                    this.headroom_db = HEADROOM_PRESETS[index];
                }),
                options: HEADROOM_PRESETS
                    .iter()
                    .map(|db| RadioItem {
                        label: format!("{db:.0} dB headroom"),
                        ..Default::default()
                    })
                    .collect(),
                ..Default::default()
            }
            .into(),
            MenuItem::Separator,
            StandardItem {
                label: "Quit Headphone Safety".into(),
                icon_name: "application-exit".into(),
                activate: Box::new(|_| std::process::exit(0)),
                ..Default::default()
            }
            .into(),
        ]
    }
}
