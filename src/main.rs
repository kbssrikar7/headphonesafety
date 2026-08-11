mod settings;
mod volume_cap;

use settings::Settings;
use std::thread;
use std::time::Duration;

const POLL_INTERVAL: Duration = Duration::from_millis(350);

fn main() {
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
