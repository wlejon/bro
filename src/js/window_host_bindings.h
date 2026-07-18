#pragma once

#include <cstdint>

// JSValue is a struct/union by value in the message notify below — the
// forward-declared JSContext alone isn't enough.
extern "C" {
#include "quickjs.h"
}

namespace bro::engine { class Engine; }

namespace bro::js {

/// Install bro.window.open(src, opts) into `ctx` (creating bro / bro.window
/// objects if the realm lacks them). Only the engine's MAIN app realm may
/// actually open windows — the function is also installed into child (iframe)
/// realms so they get a clean, deliberate error instead of a property lookup
/// failure. The opened window hosts a real, isolated document realm built from
/// `src` — see docs/window-api.js.
void installWindowHostBindings(JSContext* ctx, engine::Engine* engine);

/// Free every handle reference this context's registry holds (no 'close'
/// events fire). Call on app reload for the dying realm and at engine
/// teardown, before the context is destroyed.
void cleanupWindowHostBindings(JSContext* ctx);

/// Engine → JS: the host `id`'s document finished loading (drain point, right
/// after the realm is built and laid out — the same moment <iframe> fires its
/// element 'load'). Fires the handle's 'load' listeners. No-op for unknown ids.
void windowHostNotifyLoaded(JSContext* ctx, uint64_t id);

/// Engine → JS: a message from host `id`'s realm arrived (drain point). Fires
/// the handle's 'message' listeners with `data` on the event. TAKES OWNERSHIP
/// of `data` — freed here whether or not a handle is still registered, so a
/// message racing the window's teardown is a clean no-op.
void windowHostNotifyMessage(JSContext* ctx, uint64_t id, JSValue data);

/// Engine → JS: host `id`'s window changed client size. Fires the handle's
/// 'resize' listeners (event carries width/height). The child realm gets its
/// own window 'resize' event independently. No-op for unknown ids.
void windowHostNotifyResized(JSContext* ctx, uint64_t id, int width, int height);

/// Engine → JS: the host `id` has been destroyed (drain point). Fires the
/// handle's 'close' listeners (handle.closed already reads true) and releases
/// the registry's reference to the handle object. Safe to call for ids the
/// registry doesn't know (no-op).
void windowHostNotifyClosed(JSContext* ctx, uint64_t id);

} // namespace bro::js
