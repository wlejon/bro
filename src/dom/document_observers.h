#pragma once

// What a Document tells the layers above it (document.h's base): the C++
// half of window listeners, node-freed notices, mutation notices and the
// clone hook. Kept apart from Document's own state so the registries read in
// one place; Document fires them (document_nodes.cpp).

#include "dom/event_target.h"

#include <string>
#include <vector>

namespace bro::dom {

class Document;
class Element;
class Node;

class DocumentObservers {
public:
    // ---- window event listeners (C++ side) --------------------------------
    // One Document is one realm, and a realm has one window, so this is the
    // C++ half of `window.addEventListener` for this document's realm:
    //
    //     auto h = doc->windowListeners().add("resize", [](dom::Event&) {...});
    //     doc->windowListeners().remove(h);
    //
    // These fire from js::dispatchWindowEvent, merged with the realm's JS
    // window listeners in registration order. `window` is not an Element, so
    // the Event's target() is null for events fired at the window directly;
    // events that reach the window by bubbling out of the tree keep theirs.
    //
    // bro::engine::Engine::addWindowEventListener wraps this for the app realm.
    NativeListenerList& windowListeners() { return windowListeners_; }
    const NativeListenerList& windowListeners() const { return windowListeners_; }

    // External host layers that wrap elements (e.g. the Bronze host holds a
    // compiled-side object per element, keyed by raw Element*). Observers receive
    // node destruction notices in registration order.
    // An observer must not mutate the DOM from inside: it is called mid-tear.
    using NodeObserver = void(*)(Document*, Node*);
    void addNodeFreedObserver(NodeObserver cb);
    void removeNodeFreedObserver(NodeObserver cb);

    // ---------- Mutation notices ------------------------------------------
    //
    // What just changed in the tree, as plain data.
    //
    // notifyChildListChanged() in element.cpp says MutationObserver records
    // stay in the JS bindings because they are "per-realm observer lists, JS
    // callbacks". That is true of the DELIVERY and not of the NOTICE. A
    // function-pointer list carries no realm and no runtime-specific context, and each
    // wrapper layer above — such as the bronze host — turns the
    // same notice into its own records for its own callbacks. It is the shape
    // the live-Range notifications (Document::notifyNodeRemoved) already have, for the same reason:
    // what changed is a property of the tree, not of who changed it.
    //
    // That is the whole point of putting it here. A compiled app observing its
    // own subtree, a page script mutating that subtree, and the engine's own
    // C++ all reach the same three mutators, so an observer registered here
    // sees every mutation rather than only the ones made through one binding
    // layer.
    //
    // Fired AFTER the tree has changed, except for a removal, which fires in
    // the instant BEFORE the detach — the record names the siblings the node
    // had, and a record built afterwards would name nothing. `added`/`removed`
    // are one node per notice because the mutators move one node at a time; a
    // wrapper that wants the web's array-shaped record coalesces them itself.
    //
    // An observer must NOT mutate the DOM from inside one: it is called with
    // the tree mid-change and every other observer still to run.
    struct MutationNotice {
        enum class Kind { ChildList, Attributes, CharacterData };
        Kind kind = Kind::ChildList;
        Node* target = nullptr;   // the PARENT for ChildList, the node itself otherwise
        Node* added = nullptr;
        Node* removed = nullptr;
        Node* previousSibling = nullptr;
        Node* nextSibling = nullptr;
        // Both borrowed for the duration of the call and never stored: an
        // observer that wants either past its own return copies it.
        const std::string* attributeName = nullptr;  // Attributes only
        const std::string* oldValue = nullptr;       // Attributes / CharacterData
        // ChildList, a replace-all only: every node added / removed, in
        // order, in place of the single `added` / `removed` above.
        const std::vector<Node*>* addedList = nullptr;
        const std::vector<Node*>* removedList = nullptr;
    };
    using MutationObserverFn = void(*)(Document*, const MutationNotice&);
    void addMutationObserver(MutationObserverFn cb);
    void removeMutationObserver(MutationObserverFn cb);

    // Every firing site is guarded on this. It is not an optimisation of the
    // notify call — it is what keeps the cost of the whole feature at zero for
    // a document nobody is observing: setAttribute would otherwise copy the old
    // value out on every write, and the HTML parser would build a notice for
    // every node it appends.
    bool hasMutationObservers() const { return !mutationObservers_.empty(); }

    // Fired from cloneNode() for each cloned element, after its attributes and
    // (for a deep clone) its children are in place. Everything the DOM layer
    // can copy on its own already has been; this hook exists for the state
    // that lives ABOVE dom — the layout-owned <select> control's live
    // selection, and the canvas module's backing store — which dom must not
    // reach into directly. Set by the JS layer alongside the other hooks.
    using ElementClonedCallback = void(*)(Document*, Element* src, Element* clone);
    void setElementClonedCallback(ElementClonedCallback cb) { elementClonedCb_ = cb; }

protected:
    NativeListenerList windowListeners_;
    std::vector<NodeObserver> nodeFreedObservers_;
    std::vector<MutationObserverFn> mutationObservers_;
    ElementClonedCallback elementClonedCb_ = nullptr;
};

} // namespace bro::dom
