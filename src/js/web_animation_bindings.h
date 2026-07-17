#pragma once

// element.animate() — Web Animations API bindings.
//
// Installs the Animation class plus Element.prototype.animate /
// Element.prototype.getAnimations / Document.prototype.getAnimations. The JS
// Animation wrapper holds only the record id; records live in the engine's
// WebAnimationManager (src/engine/web_animations.h) and every wrapper→record
// resolution is id-based, so a wrapper outliving its record (cancel, document
// teardown) is inert rather than dangling.

#include <cstdint>
#include <vector>

struct JSContext;

namespace bro::js {

void installWebAnimationBindings(JSContext* ctx);

// Free the strong wrapper pins held for this context (running animations pin
// their wrapper so a finish can still be delivered). Must run before the JS
// runtime is torn down or the pinned wrappers trip the leak asserts. Called
// from DomBindings::cleanup().
void cleanupWebAnimationBindings(JSContext* ctx);

// Deliver finish events queued by WebAnimationManager::tick(): resolve each
// animation's finished promise and fire onfinish. Called on the main thread
// at the same drain points as the CSS transition/animation events.
void deliverWebAnimationFinishEvents(JSContext* ctx, std::vector<uint64_t> ids);

} // namespace bro::js
