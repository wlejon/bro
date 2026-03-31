#pragma once

#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

/// Install the customElements registry and HTMLElement constructor.
/// Must be called AFTER DomBindings::install (needs element prototype).
/// elementClassId is the QuickJS class ID used for Element wrappers.
void installCustomElements(JSContext* ctx, JSClassID elementClassId, void* documentPtr);

/// Clean up per-context custom element state. Call before destroying JSContext.
void cleanupCustomElements(JSContext* ctx);

/// If tag is a registered custom element, construct via the user's class
/// and return the JS wrapper. The C++ Element* must already be created.
/// Returns JS_UNDEFINED if tag is not a custom element.
JSValue createCustomElement(JSContext* ctx, void* elemPtr, const std::string& tag);

/// After wrapping a custom element via wrapElement, upgrade its prototype
/// to the registered class. Returns true if upgraded.
bool upgradeCustomElementPrototype(JSContext* ctx, JSValue wrapper, const std::string& tagName);

/// Fire connectedCallback on element (and custom-element descendants).
void fireConnectedCallback(JSContext* ctx, JSValue elementWrapper);

/// Fire disconnectedCallback on element (and custom-element descendants).
void fireDisconnectedCallback(JSContext* ctx, JSValue elementWrapper);

/// Fire attributeChangedCallback if the attribute is observed.
void fireAttributeChangedCallback(JSContext* ctx, JSValue elementWrapper,
                                  const std::string& name,
                                  const std::string& oldVal,
                                  const std::string& newVal);

} // namespace bro::js
