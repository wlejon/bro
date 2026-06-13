#pragma once

#include <quickjs.h>

namespace bro::js {

// bro.listen — the shared listening stream's own JS surface. Currently: opt-in
// raw-audio retention over the one shared listen host (mic today; any future
// non-mic source that drives the host too), so an app can replay/scrub the
// recent stream by frame range. See listen_host.h for the retention design and
// docs/listen-api.js for the JS reference. Inert until bro.listen.retain().
void installListenBindings(JSContext* ctx);

}  // namespace bro::js
