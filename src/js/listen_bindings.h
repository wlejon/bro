#pragma once

#include <quickjs.h>

namespace bro::js {

// bro.listen — the JS surface over the listen host (listen_host.h).
//
//   bro.listen.open(source?)  -> ListenStream handle. source: 'mic' (default) /
//                                'system' (whole-system loopback) / {process:pid,
//                                exclude?} (one app's audio) / {channel:n}. The
//                                handle exposes per-stream .retain/.audio/.frame/
//                                .info/.feed/.close; closing (or GC) frees the
//                                stream. Open many for N concurrent, unmixed
//                                streams (mic + system at once, L/R separately).
//   bro.listen.supported()    -> render-side (system/per-app) capture available?
//   bro.listen.apps()         -> [{pid,name}] apps holding a render session.
//
// The legacy global bro.listen.retain/audio/frame/info target one shared mic
// stream (migration scaffold) and will be retired once tenants move to handles.
// See docs/listen-api.js for the JS reference.
void installListenBindings(JSContext* ctx);

}  // namespace bro::js
