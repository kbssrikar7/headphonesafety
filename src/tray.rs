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
    /// The real output device, cached once at startup (hard-won lesson #3: cache identifiers
    /// while everything is healthy, rather than re-resolving "the default sink" later — once the
    /// limiter is routed, the default sink *is* our virtual sink, not the real device).
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

        match limiter::load(&self.master_sink, self.headroom_db, "hps_limiter") {
            Ok(route) => match routing::set_default(&route.sink_name) {
                Ok(()) => {
                    self.status = format!("Real-Time Limiter on ({:.0} dB headroom)", self.headroom_db);
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
        vec![
            StandardItem {
                label: format!("Device: {}", self.master_sink),
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
