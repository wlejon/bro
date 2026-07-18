#pragma once

// window.matchMedia() — MediaQueryList bindings.
//
// Installs the MediaQueryList class plus the matchMedia global (window ===
// globalThis in every realm). Evaluation reuses htmlayout's media-query
// evaluator against the realm document's MediaContext — the exact same
// parser/context that filters @media blocks, so matchMedia and CSS can never
// disagree. A MediaQueryList with registered listeners is strong-pinned for
// the life of its realm (browser behavior: dropping the app reference must
// not silence change events); listener-less lists GC normally.

struct JSContext;

namespace bro::js {

void installMatchMediaBindings(JSContext* ctx);

// Free the strong wrapper pins + per-realm bookkeeping for this context.
// Must run before the JS runtime is torn down or the pinned wrappers trip
// the leak asserts. Called from DomBindings::cleanup().
void cleanupMatchMediaBindings(JSContext* ctx);

// Re-evaluate every live MediaQueryList of this realm against its document's
// current MediaContext and fire 'change' on the ones whose matches flipped.
// Cheap no-op unless the document's media generation moved; defers while the
// media-triggered restyle is still pending so listeners always observe styles
// consistent with the new context. Main thread only — called at the same
// post-restyle drain points as the CSS transition/animation events.
void deliverMediaQueryChanges(JSContext* ctx);

} // namespace bro::js
