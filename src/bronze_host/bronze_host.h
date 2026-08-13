#pragma once

// Public surface of the bronze host layer (src/bronze_host/README.md): the
// one call a host executable makes between constructing the Engine and
// running the compiled program.

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

/// Register the browser-shaped host globals a bronze-compiled three.js app
/// reads — document, window, self, requestAnimationFrame,
/// cancelAnimationFrame, performance, WebGL2RenderingContext — and hook the
/// requestAnimationFrame queue into `engine`'s frame loop (Engine::onFrame).
///
/// The registered names match src/bronze_host/threejs_host.globals line for
/// line; the app must have been compiled with that manifest
/// (`--host-globals`) for its reads to reach the registry at all.
///
/// Call AFTER the Engine exists and BEFORE bronze::embed::runMain(): the
/// program's top level runs inside runMain, and a global it reads must
/// already be registered. Once per process — the engine pointer and the
/// frame hook live for the process, matching Engine::onFrame's
/// register-once convention.
void installThreejsHostGlobals(engine::Engine& engine);

}  // namespace bro::bronze_host
