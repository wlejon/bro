#pragma once

// Public surface of the bronze host layer (src/bronze_host/README.md): the
// one call a host executable makes between constructing the Engine and
// running the compiled program.

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

/// Register the browser-shaped host globals a bronze-compiled app
/// reads — document, window, self, requestAnimationFrame,
/// cancelAnimationFrame, performance, WebGL2RenderingContext, the four timer
/// functions, Image, XMLHttpRequest, fetch, Request, Headers, Response — and hook the frame seam into
/// `engine`'s frame loop (Engine::onFrame). That seam is what advances the
/// clock, delivers host completions, fires timers and rAF, and performs the
/// microtask checkpoint the compiled program's promises need; without it a
/// promise queued after the top level would never run.
///
/// The registered names match src/bronze_host/web_host.globals line for
/// line; the app must have been compiled with that manifest
/// (`--host-globals`) for its reads to reach the registry at all.
///
/// Call AFTER the Engine exists and BEFORE bronze::embed::runMain(): the
/// program's top level runs inside runMain, and a global it reads must
/// already be registered. Once per process — the engine pointer and the
/// frame hook live for the process, matching Engine::onFrame's
/// register-once convention.
void installWebHostGlobals(engine::Engine& engine);

/// Backwards compatibility alias for installWebHostGlobals.
inline void installThreejsHostGlobals(engine::Engine& engine) {
    installWebHostGlobals(engine);
}

}  // namespace bro::bronze_host
