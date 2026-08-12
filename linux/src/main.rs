mod limiter;
mod pactl;
mod pw_client;
mod routing;
mod settings;
mod tray;
mod volume_cap;

use ksni::blocking::TrayMethods;
use settings::Settings;
use std::process::Command;
use std::thread;
use std::time::Duration;
use tray::AppTray;

const POLL_INTERVAL: Duration = Duration::from_millis(350);

fn main() {
    match std::env::args().nth(1).as_deref() {
        Some("test-routing") => return test_routing(),
        Some("test-limiter") => return test_limiter(),
        Some("test-watcher") => return test_watcher(),
        _ => {}
    }
    run_daemon();
}

/// The long-running daemon: tray UI, Volume Cap poll loop, and the device-removal watcher's
/// auto-revert, all coordinated through the tray's `Handle` (see `tray.rs` for why that's the
/// single source of truth instead of a separate `Arc<Mutex<_>>`).
fn run_daemon() {
    if let Err(e) = routing::cleanup_stray_routes() {
        eprintln!("startup cleanup check failed (continuing anyway): {e}");
    }

    let settings = Settings::from_env();
    let master = pactl::default_sink_name().expect("read default sink at startup");
    println!(
        "headphonesafety: starting (device: {master}, headroom {:.1} dB below max, polling every {}ms)",
        settings.headroom_db,
        POLL_INTERVAL.as_millis()
    );

    let app_tray = AppTray::new(master, settings.headroom_db, settings.volume_cap_enabled);
    let handle = app_tray.spawn().expect("spawn tray service");

    let removals = pw_client::spawn_sink_removal_watcher();

    loop {
        let (cap_enabled, headroom, limiter_master) = handle
            .update(|tray: &mut AppTray| {
                (
                    tray.volume_cap_enabled,
                    tray.headroom_db,
                    tray.limiter_route.as_ref().map(|r| r.master_sink.clone()),
                )
            })
            .unwrap_or((settings.volume_cap_enabled, settings.headroom_db, None));

        // Cap the real device's volume. While the limiter is routed, the default sink *is* the
        // virtual routing sink, not the real device, so target the limiter's cached master
        // instead of re-asking pactl for "the default sink" in that case.
        if cap_enabled {
            let target = match &limiter_master {
                Some(m) => Ok(m.clone()),
                None => pactl::default_sink_name(),
            };
            match target.and_then(|t| volume_cap::enforce_cap(&t, headroom)) {
                Ok(Some(event)) => {
                    let status = format!(
                        "clamped {}: {:.1} dB -> {:.1} dB",
                        event.sink_name, event.from_db, event.to_db
                    );
                    println!("{status}");
                    handle.update(|tray: &mut AppTray| tray.status = status);
                }
                Ok(None) => {}
                Err(e) => eprintln!("volume_cap tick failed (will retry next poll): {e}"),
            }
        }

        // React to any device removal matching whatever we're currently routed through — hard-won
        // lesson #4: react unconditionally on the cached name, never re-query the vanished device.
        while let Ok(removed_name) = removals.try_recv() {
            if limiter_master.as_deref() == Some(removed_name.as_str()) {
                println!("device removed while routed ({removed_name}) — reverting to a fallback device");
                handle.update(|tray: &mut AppTray| {
                    if let Some(route) = tray.limiter_route.take() {
                        tray.status = match routing::revert_to_fallback(&route) {
                            Ok(fallback) => format!("Real-Time Limiter off (device removed, fell back to {fallback})"),
                            Err(e) => format!("device removed and fallback failed: {e}"),
                        };
                        tray.limiter_enabled = false;
                    }
                });
            }
        }

        thread::sleep(POLL_INTERVAL);
    }
}

/// One-shot manual verification of the routing plumbing (build-order step 3): load a passthrough
/// sink over the real default sink, switch to it, hold briefly, then revert. Run with `pactl -f
/// json list sink-inputs` showing nothing active first, same precaution as the live Volume Cap
/// test, since this switches the machine's default output for a few seconds.
fn test_routing() {
    let master = pactl::default_sink_name().expect("read default sink");
    println!("master sink: {master}");

    let route = routing::load_passthrough(&master, "hps_passthrough").expect("load passthrough");
    println!("loaded module {} -> sink {}", route.module_id, route.sink_name);

    routing::set_default(&route.sink_name).expect("switch to passthrough");
    println!("default sink is now {} (readback-confirmed)", route.sink_name);

    thread::sleep(Duration::from_secs(2));

    routing::revert(&route).expect("revert to master");
    println!("reverted to {} and unloaded module (readback-confirmed)", master);
}

/// Manual verification of the Real-Time Limiter (build-order step 4): loads the ZaMaximX2-backed
/// limiter at a -10dB headroom preset, plays a deliberately loud (boosted, clipping-range) test
/// tone through it, and logs the recorded peak dB before/after — per docs/ubuntu-port.md's
/// testing checklist, this must be verified by logged levels, not "sounds a bit quieter" by ear.
/// Temporarily sets the real device's volume to 100% so the measurement isn't confounded by
/// Volume Cap's own attenuation, and restores it afterward.
fn test_limiter() {
    let headroom_db = 10.0;
    let master = pactl::default_sink_name().expect("read default sink");
    let orig_volume = pactl::list_sinks()
        .expect("list sinks")
        .into_iter()
        .find(|s| s["name"] == master)
        .and_then(|s| s["volume"]["front-left"]["value_percent"].as_str().map(str::to_owned))
        .expect("read master volume");
    println!("master sink: {master} (original volume {orig_volume}, headroom {headroom_db} dB)");

    pactl::run(&["set-sink-volume", &master, "100%"]).expect("set master to 100% for clean measurement");

    let route = limiter::load(&master, headroom_db, "hps_limiter").expect("load limiter");
    println!("loaded module {} -> sink {}", route.module_id, route.sink_name);
    routing::set_default(&route.sink_name).expect("switch to limiter");

    let tone = "/tmp/hps_test_tone.wav";
    let recorded = "/tmp/hps_test_recorded.wav";
    Command::new("ffmpeg")
        .args([
            "-y", "-f", "lavfi", "-i", "sine=frequency=440:duration=3:sample_rate=48000",
            "-af", "volume=20dB", "-ac", "2", tone,
        ])
        .output()
        .expect("generate test tone");
    let source_peak = measure_peak_db(tone).expect("measure source peak");
    println!("source tone peak: {source_peak:.1} dB (before limiting)");

    // Warm up the ALSA sink from SUSPENDED before the timed recording window.
    Command::new("paplay").args(["--device", &route.sink_name, tone]).status().ok();

    let mut rec = Command::new("parecord")
        .args(["--file-format=wav", "--device", &format!("{master}.monitor"), recorded])
        .spawn()
        .expect("start parecord");
    thread::sleep(Duration::from_secs(1));
    Command::new("paplay").args(["--device", &route.sink_name, tone]).status().expect("play test tone");
    thread::sleep(Duration::from_secs(1));
    interrupt(rec.id());
    rec.wait().ok();

    let recorded_peak = measure_peak_db(recorded).expect("measure recorded peak");
    println!("recorded peak after limiting: {recorded_peak:.1} dB (ceiling was {:.1} dB)", -headroom_db);
    if recorded_peak > -headroom_db + 1.0 {
        eprintln!("WARNING: recorded peak exceeds the ceiling by more than 1dB tolerance — limiter may not be capping correctly");
    } else {
        println!("OK: recorded peak holds at/below the ceiling (within tolerance)");
    }

    routing::revert(&route).expect("revert to master");
    pactl::run(&["set-sink-volume", &master, &orig_volume]).expect("restore master volume");
    println!("reverted to {master} and restored volume to {orig_volume}");

    std::fs::remove_file(tone).ok();
    std::fs::remove_file(recorded).ok();
}

/// Sends SIGINT (not SIGTERM) so `parecord` finalizes the WAV file's data/duration cleanly on
/// exit instead of leaving a header-only truncated file — confirmed live during step 3 testing.
fn interrupt(pid: u32) {
    Command::new("kill").args(["-INT", &pid.to_string()]).status().ok();
}

/// Manual verification of the native `pipewire` registry watcher (build-order step 5, device-
/// tracking half): confirms the event-driven removal detection actually fires on a real removal,
/// without ever touching the machine's default sink (unlike test-routing/test-limiter, this test
/// never calls `set-default-sink`, so it's safe to run even with audio actively playing).
///
/// Loads a disposable null-sink, gives the watcher time to observe its `global` event and cache
/// its name, unloads it, then waits for the corresponding removal event — proving detection is
/// real (a `global_remove` for an id the watcher's cache actually held), not just "the channel
/// didn't error."
fn test_watcher() {
    let rx = pw_client::spawn_sink_removal_watcher();
    println!("watcher thread started, connecting to pipewire...");
    thread::sleep(Duration::from_millis(500));

    let sink_name = "hps_watch_test";
    let module_id = pactl::run(&["load-module", "module-null-sink", &format!("sink_name={sink_name}")])
        .expect("load test null-sink")
        .trim()
        .to_string();
    println!("loaded disposable null-sink {sink_name} (module {module_id})");

    // Give the watcher time to receive and cache this node's `global` event before it's removed.
    thread::sleep(Duration::from_millis(500));

    // Nothing should have been reported yet — the sink is still present.
    match rx.try_recv() {
        Ok(name) => eprintln!("WARNING: unexpected removal event for {name} before anything was removed"),
        Err(_) => println!("OK: no spurious removal event while the sink still exists"),
    }

    pactl::run(&["unload-module", &module_id]).expect("unload test null-sink");
    println!("unloaded {sink_name}, waiting for the watcher to notice...");

    let deadline = Duration::from_secs(3);
    let start = std::time::Instant::now();
    loop {
        match rx.recv_timeout(deadline.saturating_sub(start.elapsed())) {
            Ok(name) if name == sink_name => {
                println!("OK: watcher reported removal of {name} (event-driven, no polling)");
                return;
            }
            Ok(other) => println!("(ignoring removal event for unrelated sink {other})"),
            Err(_) => {
                eprintln!("FAIL: no removal event for {sink_name} within {deadline:?}");
                return;
            }
        }
    }
}

fn measure_peak_db(path: &str) -> Option<f64> {
    let output = Command::new("ffmpeg")
        .args(["-i", path, "-af", "volumedetect", "-f", "null", "-"])
        .output()
        .ok()?;
    let stderr = String::from_utf8_lossy(&output.stderr);
    stderr.lines().find_map(|line| {
        let line = line.trim();
        let rest = line.strip_prefix("[Parsed_volumedetect_0")?;
        let (_, after) = rest.split_once("max_volume:")?;
        after.trim().trim_end_matches(" dB").parse::<f64>().ok()
    })
}
