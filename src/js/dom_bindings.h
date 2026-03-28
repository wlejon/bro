#pragma once

#include <functional>
#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::dom { class Element; }

namespace bro::js {

class DomBindings {
public:
    /// Callback for element.getContext(type) — lets the engine handle canvas creation.
    using GetContextFactory = std::function<JSValue(JSContext*, bro::dom::Element*, const std::string&)>;
    static void setGetContextFactory(GetContextFactory factory);
    /// Register all DOM classes (Document, Element, Event, CSSStyleDeclaration,
    /// NodeList) and set the global `document` property wrapping the given
    /// Document pointer.
    static void install(JSContext* ctx, void* document_ptr);

    // -----------------------------------------------------------------------
    // Helpers exposed so other modules can wrap / unwrap DOM objects.
    // -----------------------------------------------------------------------

    /// Wrap a bro::dom::Element* into a JS Element object.
    static JSValue wrapElement(JSContext* ctx, void* element_ptr);

    /// Unwrap a JS Element object back to bro::dom::Element*.
    static void* unwrapElement(JSContext* ctx, JSValueConst val);

    /// Wrap a bro::dom::Document* into a JS Document object.
    static JSValue wrapDocument(JSContext* ctx, void* document_ptr);

    /// Free static prototypes to allow clean JS runtime shutdown.
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
