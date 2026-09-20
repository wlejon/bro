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
namespace bro::dom {
class Document;
class Element;
}

namespace bro::bronze_host {

/// Register the browser-shaped host globals a bronze-compiled app
/// reads — document, window, self, requestAnimationFrame,
/// cancelAnimationFrame, performance, WebGL2RenderingContext, the four timer
/// functions, Image — and hook the frame seam into
/// `engine`'s frame loop (Engine::onFrame). That seam is what advances the
/// clock, delivers host completions, fires timers and rAF, and performs the
/// microtask checkpoint the compiled program's promises need; without it a
/// promise queued after the top level would never run.
///
/// There is no hand-kept list of what this registers: registeredHostGlobals()
/// below reads the names back off bronze's registry, and that enumeration is
/// what every compile — in-process (`EvalOptions::hostGlobals`) or ahead of
/// time (`bro-headless <app> --print-host-globals`, piped to `bronze build
/// --host-globals`) — is given. A name a compiled program reads must be in
/// that list AND registered, and with one source for both it cannot be one
/// without the other.
///
/// Call AFTER the Engine exists and BEFORE bronze::embed::runMain(): the
/// program's top level runs inside runMain, and a global it reads must
/// already be registered. Once per process — the engine pointer and the
/// frame hook live for the process, matching Engine::onFrame's
/// register-once convention.
void installWebHostGlobals(engine::Engine& engine);

/// Whether installWebHostGlobals has already been called on this process.
bool isWebHostGlobalsInstalled();

/// Every host global registered on the CALLING thread, in registration
/// order — bronze's registry is per-thread, so on the main thread this is
/// what installWebHostGlobals (and anything registered after it) put in, and
/// on a worker thread it is that worker's own realm. This is the compile-time
/// half of the host-globals contract, taken from the run-time half rather
/// than typed beside it.
std::vector<std::string> registeredHostGlobals();

/// The other half of the same contract: the natives registered on the
/// CALLING thread (the `__bro_native.*` entry points host_bro_root.cpp
/// registers inside installWebHostGlobals), written as the JSON manifest
/// `bronze build --native-manifest` reads — bronze::embed::writeNativeManifest
/// over the live registry. `bro-headless <app> --print-native-manifest
/// <path>` is this, for an ahead-of-time compile; the in-process compiles
/// write it themselves (eval.cpp). False with `*error` set if the file
/// cannot be written.
bool writeNativeManifest(const std::string& path, std::string* error);

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
void clearParsedDocuments();
void clearParsedDocumentsForScope(uint64_t scopeId);

/// Canvas cleanup across element destruction and app reload.
void clearHostCanvases();
void cleanupCanvasForElement(dom::Element* el);

/// DOM event listener cleanup.
void clearAllElementListeners();
void clearElementListeners(dom::Element* el);
void clearElementListenersForDocument(dom::Document* doc);

/// Custom elements registry reset.
void resetCustomElementsRegistry();

/// Dynamic module compilation cache and temporary module cleanup.
void clearDynamicModules();

/// Deliver media query changes to matchMedia listeners.
void deliverHostMediaQueryChanges();
void clearHostMediaQueries();

/// Clear persistent menu callbacks on app reload.
void clearMenuHandlers();

/// Terminate and join all active worker threads on shutdown.
void terminateAllWorkers();

/// Run sub-document scripts within its document scope and realm gating.
void runHostSubDocScripts(engine::Engine& engine, dom::Document* subDoc,
                          const std::vector<engine::ScriptEntry>& scripts,
                          const std::string& appDir, const std::string& basePath,
                          bool isChild);

/// Notify document that all system panels are ready.
void triggerPanelsReady(dom::Document* doc);

/// Notify splash document to begin dismiss animation.
void triggerSplashDismiss(dom::Document* doc);

}  // namespace bro::bronze_host
