// Shared sub-document core — the build / teardown / record / replay / capture
// machinery behind every isolated document the engine hosts inside another one.
//
// Two things use it today:
//   • Engine::IframeDoc  — an <iframe>'s sub-document, laid out at the host
//     element's content box and composited as a quad in the app's layer list.
//   • Engine::WindowHost — a secondary OS window's document (bro.window.open),
//     laid out at the window's client size and composited fullscreen into that
//     window's drawable.
//
// They are NOT unified into one struct. Each keeps the members its own owner
// needs (an <iframe> back-pointer vs. an SDL window + focus flags), and the
// shared code operates on a SubDocRef — a by-reference VIEW of the members the
// two have in common. That keeps the sharp edges (member DESTRUCTION ORDER, and
// the rule that a GPU surface may only be destroyed on the context that made
// it) visible in the owning struct where they matter, instead of hidden behind
// a base class.

#pragma once

#include "engine/engine.h"
#include "engine/app_loader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::engine {

/// By-reference view of the members every hosted sub-document shares. Cheap to
/// build (a pack of references), never stored — construct one at the call site
/// from the owning struct and pass it in.
struct SubDocRef {
    JSContext*& jsCtx;
    std::unique_ptr<js::Timers>& timers;
    std::vector<std::unique_ptr<canvas::CanvasScene>>& canvasScenes;
    std::unique_ptr<dom::Document>& document;
    dom::Element*& hoveredElement;
    int& boxW;
    int& boxH;
    render::CommandBuffer& cmdBuffer;
    render::SkiaRenderer::GPUSurface& surface;
    int& surfW;
    int& surfH;
    unsigned int& fboTexture;
};

/// A sub-app loaded off disk: everything createIframeDoc / the window-host
/// build need before they touch the sub-document's realm. `ok` is false when
/// the src doesn't resolve to a bro app (the loader has already logged why).
struct SubDocSource {
    bool ok = false;
    std::string appDir;       // directory the app was loaded from
    std::string resolvedSrc;  // src resolved against the parent's base path
    AppManifest manifest;
    std::string html;
    std::string authorStyles;
};

/// Resolve `srcAttr` against `basePath` (honouring asset mounts) and load the
/// bro app it names: a directory, or a file whose directory is the app. A src
/// that doesn't exist FAILS rather than falling back to its parent directory —
/// that fallback would silently embed the parent app inside itself. `what`
/// names the caller in log messages ("iframe", "bro.window.open").
SubDocSource loadSubDocSource(const std::string& basePath, const std::string& srcAttr,
                              const util::AssetMounts* mounts, const char* what);

/// Parse the loaded HTML (templates extracted first, like the top-level app
/// path) into a fresh Document whose media context is the sub-doc's own box.
void buildSubDocDocument(SubDocRef d, const SubDocSource& src,
                         const std::string& colorScheme);

/// What the shared realm build needs to know about its host.
struct SubDocRealmOptions {
    /// The platform window this realm's globals describe. Iframes and system
    /// panels pass the PRIMARY window; a secondary-window host passes ITS OWN
    /// window, which is what makes window.screen / bro.window.getPosition()
    /// answer for the right window inside a host realm.
    platform::Window* window = nullptr;
    double displayScale = 1.0;
    /// The engine's current scaled-clock timestamp. The sub-doc's Timers are
    /// primed with it so a setTimeout registered during the sub-app's startup
    /// scripts is relative to the SAME clock tick() later uses. Without this
    /// the delay is measured from wall-clock time while the ticks arrive on
    /// the (headless: virtual) engine clock, and the timer fires late or never
    /// — the slower the load, the wider the gap.
    double nowMs = 0.0;
    bool headless = false;
    /// Install the scoped bro.window.* surface (state/position/title/…) acting
    /// on `window`. Secondary-window hosts only — an <iframe> has no window of
    /// its own to manage.
    bool installBroWindow = false;
    Settings* settings = nullptr;
    /// Warn (once per canvas) when the sub-app asks for a WebGL context. v1
    /// refuses WebGL in secondary windows; getContext() returns null either way.
    bool warnOnWebGL = false;
    const char* what = "sub-document";
};

/// Create the sub-document's JSContext and install the standard, isolated app
/// bindings on it: console, timers, window globals, bro.window.open (which
/// throws its deliberate main-realm error here), DOM, storage, settings,
/// canvas/image bindings, and the 2D getContext factory that parks each
/// <canvas>'s scene in THIS sub-doc.
void buildSubDocRealm(SubDocRef d, js::Runtime* runtime, Engine* engine,
                      const SubDocSource& src, render::Renderer* renderer,
                      const SubDocRealmOptions& o);

/// Run the sub-app's scripts on its own context. Each sub-document has its own
/// JSContext (not the runtime's shared module context), so only classic scripts
/// run — type="module" is skipped with a warning.
void runSubDocScripts(SubDocRef d, const SubDocSource& src, const char* what);

/// Replaced elements + the first style/layout pass at the box size.
void finishSubDocLoad(SubDocRef d, const SubDocSource& src,
                      render::Renderer* renderer, broaudio::Engine* audio,
                      layout::SkiaTextMetrics& metrics);

/// Log a clear warning for each <iframe> in a sub-document that cannot host
/// one, and leave it unloaded. syncIframes() only walks the app document, so a
/// nested <iframe> is inert rather than broken — but silently inert is worse
/// than a warning.
void warnNestedIframes(SubDocRef d, const char* what);

/// Advance this sub-document's timers + requestAnimationFrame callbacks and
/// report whether it needs (re)recording this frame — never recorded, DOM
/// changed, or animating. Sub-doc activity has no other route to the raster
/// thread; without it the sub-doc never records and its texture stays 0.
bool tickSubDoc(SubDocRef d, double nowMs);

/// Main thread: style + lay the sub-document out at its box, stage its canvas
/// commands, and record its paint into `cmdBuffer`.
void recordSubDoc(SubDocRef d, render::RecordingRenderer* rec,
                  layout::DrawTraversal* traversal, layout::SkiaTextMetrics& metrics);

/// Raster thread: replay `cmdBuffer` into a box-sized GPU surface (created or
/// resized here) and publish its texture as `fboTexture`. The surface belongs
/// to `renderer`'s context from here on.
void replaySubDoc(SubDocRef d, render::SkiaRenderer* renderer);

/// Synchronous main-thread capture: replay `cmdBuffer` into a throwaway surface
/// on `skia` and read the pixels back (top-down RGBA). Returns {} if there is
/// nothing recorded. The caller is responsible for having brought the sub-doc
/// current (quiesce raster → record) first.
std::vector<uint8_t> captureSubDoc(SubDocRef d, render::SkiaRenderer* skia,
                                   int& outW, int& outH);

/// Tear one sub-document down: timers → DOM bindings → document (fires Element
/// finalizers into the still-live canvasScenes) → JSContext. canvasScenes are
/// left alone — they are a member declared BEFORE `document`, so they destruct
/// after it when the owning struct is finally erased.
///
/// Deliberately does NOT touch `surface`: it belongs to whichever GL context
/// replayed the sub-doc, which is not necessarily the calling thread's. Every
/// caller must have already routed it (Engine::queueIframeSurfaceFree) or know
/// the owning context already released it.
void teardownSubDoc(SubDocRef d);

} // namespace bro::engine
