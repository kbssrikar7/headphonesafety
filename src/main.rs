mod pactl;
mod routing;
mod settings;
mod volume_cap;

use settings::Settings;
use std::thread;
use std::time::Duration;

const POLL_INTERVAL: Duration = Duration::from_millis(350);

fn main() {
    if std::env::args().nth(1).as_deref() == Some("test-routing") {
        test_routing();
        return;
    }

    let settings = Settings::from_env();
    println!(
        "headphonesafety: Volume Cap prototype running (headroom {:.1} dB below max, polling every {}ms)",
        settings.headroom_db,
        POLL_INTERVAL.as_millis()
    );

    loop {
        if settings.volume_cap_enabled {
            match volume_cap::enforce_cap(settings.headroom_db) {
                Ok(Some(event)) => println!(
                    "clamped {}: {:.2} dB -> {:.2} dB",
                    event.sink_name, event.from_db, event.to_db
                ),
                Ok(None) => {}
                Err(e) => eprintln!("volume_cap tick failed (will retry next poll): {e}"),
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
