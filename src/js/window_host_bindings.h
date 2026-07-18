#pragma once

#include <cstdint>

struct JSContext;

namespace bro::engine { class Engine; }

namespace bro::js {

/// Install bro.window.open(src, opts) into `ctx` (creating bro / bro.window
/// objects if the realm lacks them). Only the engine's MAIN app realm may
/// actually open windows — the function is also installed into child (iframe)
/// realms so they get a clean, deliberate error instead of a property lookup
/// failure. v1 IN PROGRESS: the opened window is a blank clear-color surface;
/// its document lands with the next multiwindow chunk (docs/window-api.js).
void installWindowHostBindings(JSContext* ctx, engine::Engine* engine);

/// Free every handle reference this context's registry holds (no 'close'
/// events fire). Call on app reload for the dying realm and at engine
/// teardown, before the context is destroyed.
void cleanupWindowHostBindings(JSContext* ctx);

/// Engine → JS: the host `id` has been destroyed (drain point). Fires the
/// handle's 'close' listeners (handle.closed already reads true) and releases
/// the registry's reference to the handle object. Safe to call for ids the
/// registry doesn't know (no-op).
void windowHostNotifyClosed(JSContext* ctx, uint64_t id);

} // namespace bro::js
