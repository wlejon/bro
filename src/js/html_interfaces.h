#pragma once

#include <quickjs.h>
#include <string>

namespace bro::js {

/// Install the built-in HTML element interface objects — HTMLCanvasElement,
/// HTMLImageElement, HTMLDivElement and the rest — and chain their prototypes
/// onto HTMLElement.prototype, which already sits on Element.prototype.
///
/// bro implements every tag with one `Element` C++ class, so without this the
/// whole HTML interface hierarchy is missing from JS: `canvas instanceof
/// HTMLCanvasElement` throws ReferenceError rather than answering true, and
/// `div instanceof HTMLElement` is false. Libraries branch on exactly those
/// tests — three.js decides whether an image is serializable by asking
/// `image instanceof HTMLCanvasElement`, so a failing guard silently drops
/// every texture from a saved scene rather than raising anything.
///
/// Only the prototype chain is per-tag; the methods and accessors all still
/// live on Element.prototype, so an interface prototype is an (almost) empty
/// object whose job is to answer `instanceof` and name the constructor.
///
/// Must run after installCustomElements(), which is what creates HTMLElement.
void installHtmlInterfaces(JSContext* ctx, JSClassID elementClassId);

/// The interface prototype a tag's wrapper should carry, or JS_UNDEFINED when
/// the tag has no dedicated interface (HTMLElement covers it) or the
/// interfaces were never installed. Borrowed — do not free.
JSValue htmlInterfaceProto(JSContext* ctx, const std::string& tagName);

void cleanupHtmlInterfaces(JSContext* ctx);

} // namespace bro::js
