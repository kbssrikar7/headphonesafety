//! Native `pipewire` crate client: watches the registry for Audio/Sink node removals and reports
//! them, event-driven, with no polling.
//!
//! This is the piece the `pactl`-shell-out prototype structurally cannot provide: reacting to a
//! device disappearing (e.g. Bluetooth headphones disconnecting) via the registry's
//! `global_remove` event, per docs/ubuntu-port.md's hard-won lessons #3/#4 — cache identifiers
//! while everything is healthy, then react to removal unconditionally without querying the
//! (possibly already-gone, possibly hanging) device.
//!
//! Deliberately scoped to detection only for now (build-order step 5's device-tracking half —
//! Volume Cap stays on `pactl` for now, since it already works correctly and gains nothing
//! safety-wise from migrating first). Runs on its own dedicated OS thread, since pipewire-rs'
//! types are `Rc`-based and not `Send`; `main_loop.run()` blocks forever, so it cannot share a
//! thread with the existing poll loop.

use pipewire as pw;
use pw::types::ObjectType;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;
use std::sync::mpsc;
use std::thread;

/// Spawns the watcher thread and returns a receiver that yields the `node.name` of every
/// Audio/Sink node removed from the PipeWire graph, for as long as the process runs.
///
/// Broadcasting every removal (rather than taking a single "name to watch" argument) keeps this
/// module decoupled from routing/limiter policy — callers filter for the name they care about
/// (e.g. the currently-routed `master_sink`) rather than this module needing to know it.
pub fn spawn_sink_removal_watcher() -> mpsc::Receiver<String> {
    let (tx, rx) = mpsc::channel();

    thread::spawn(move || {
        pw::init();
        run_watcher(tx);
        // SAFETY: this thread's pipewire objects (main loop, context, core, registry, listener)
        // have all been dropped by the time `run_watcher` returns (`main_loop.run()` blocks for
        // the process lifetime, so in practice this only runs on process exit).
        unsafe {
            pw::deinit();
        }
    });

    rx
}

fn run_watcher(tx: mpsc::Sender<String>) {
    let main_loop = pw::main_loop::MainLoopRc::new(None).expect("create pipewire main loop");
    let context = pw::context::ContextRc::new(&main_loop, None).expect("create pipewire context");
    let core = context.connect_rc(None).expect("connect to pipewire");
    let registry = core.get_registry_rc().expect("get pipewire registry");

    // id -> node.name, populated from `global` while the node is present. `global_remove` only
    // hands back a bare id (the object is already gone by then), so this cache is the only way
    // to know which sink just disappeared.
    let names: Rc<RefCell<HashMap<u32, String>>> = Rc::new(RefCell::new(HashMap::new()));

    let names_for_global = names.clone();
    let names_for_remove = names.clone();

    let _registry_listener = registry
        .add_listener_local()
        .global(move |obj| {
            if obj.type_ != ObjectType::Node {
                return;
            }
            let Some(props) = obj.props else { return };
            if props.get("media.class") != Some("Audio/Sink") {
                return;
            }
            if let Some(name) = props.get("node.name") {
                names_for_global.borrow_mut().insert(obj.id, name.to_string());
            }
        })
        .global_remove(move |id| {
            if let Some(name) = names_for_remove.borrow_mut().remove(&id) {
                let _ = tx.send(name);
            }
        })
        .register();

    main_loop.run();
}
