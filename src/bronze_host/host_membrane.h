#pragma once

// What crosses between apps (host_compartment.h, sameApp): nothing mutable.
// Compartments of one app (its document, frames, windows, panels) reach one
// another's windows directly, as same-origin frames do.
//
//   - Messages. Another app's window is reached only as a WindowProxy-shaped
//     object with postMessage (and `closed`): an appview's `contentWindow`,
//     and inside it `parent` and `top`. A post is
//     serialized to bytes at the call and read back in the receiver's realm
//     (host_window_message.h), its `source` the proxy for the sender there.
//
//   - Capabilities. A host page grants an app it runs inline a function
//     (appview.expose(name, fn)); the app sees it as `bro.app.host[name]`, a
//     function of ITS realm. Called, its arguments are cloned into the host's
//     realm, the host's function runs there, and its result is cloned back (a
//     promise settles a promise of the caller's realm with the clone). A
//     function passed as an argument becomes a capability the other way, so
//     a callback the app hands the host runs in the app, its arguments cloned
//     in. Errors cross as clones too. The membrane holds no object of either
//     side the other can touch.

#include "bronze_host/host_class.h"

#include <cstdint>
#include <string>

namespace bro::bronze_host {

struct Compartment;

/// The object standing for `target`'s window in the CURRENT compartment:
/// postMessage and `closed` only. One per (viewer, target), dropped with
/// either.
Value windowProxyFor(Compartment* target);
/// `parent` and `top` of `c` (an iframe's, an appview's), installed in its
/// realm (the current one).
void installParentWindowProxy(Compartment& c);
/// The proxies a compartment made or is reached by (0: every one).
void clearWindowProxies(uint64_t compartmentId);

/// `fn`, a function of `owner`'s realm, as a function of `to`'s realm that
/// crosses the membrane (above). Made in `to` (entered here).
Value makeCapability(Compartment* owner, Value fn, Compartment* to);

/// Clone `v` out of `from`'s realm into `to`'s: structured clone by bytes,
/// with each function at the top level made a capability. Throws a
/// DataCloneError (in the current realm) for what cannot be cloned.
Value cloneAcross(Value v, Compartment* from, Compartment* to);

/// The app's `bro.app.host`, where the capabilities its host page granted
/// appear: made in `c`'s realm at install, filled by expose().
void installHostCapabilities(Compartment& c, Value broApp);
/// Grant `fn` (a function of `owner`'s, the host page) to `to` under `name`.
void exposeCapability(Compartment* owner, Compartment* to, const std::string& name, Value fn);

}  // namespace bro::bronze_host
