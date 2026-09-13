#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Public surface of the bronze host layer (src/bronze_host/README.md): the
// one call a host executable makes between constructing the Engine and
// running the compiled program.

namespace bro::engine {
class Engine;
struct ScriptEntry;
}
namespace bro::dom { class Document; }

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

/// Whether installWebHostGlobals has already been called on this process.
bool isWebHostGlobalsInstalled();

/// Clear active setTimeout and setInterval timers and tasks on reload.
void clearHostTimers();

/// Reset expandos added to globalThis across an app reload.
void resetGlobalExpandos();

/// Active host document tracking for sub-documents (iframes, window hosts).
dom::Document* currentHostDocument();
void setCurrentHostDocument(dom::Document* doc);
void clearHostDocument(dom::Document* doc);
void clearHostElementsForDocument(dom::Document* doc);
void clearHostTimersForDocument(dom::Document* doc);
void clearHostAnimationFramesForDocument(dom::Document* doc);
uint64_t scopeIdForDocument(dom::Document* doc);
void clearRealmScope(uint64_t scopeId);

/// Deliver media query changes to matchMedia listeners.
void deliverHostMediaQueryChanges();

/// Terminate and join all active worker threads on shutdown.
void terminateAllWorkers();

/// Run sub-document scripts within its document scope and realm gating.
void runHostSubDocScripts(engine::Engine& engine, dom::Document* subDoc,
                          const std::vector<engine::ScriptEntry>& scripts,
                          const std::string& appDir, const std::string& basePath,
                          bool isChild);

/// Backwards compatibility alias for installWebHostGlobals.
inline void installThreejsHostGlobals(engine::Engine& engine) {
    installWebHostGlobals(engine);
}

}  // namespace bro::bronze_host
