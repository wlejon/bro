#pragma once

// The node registry (host_node_registry.cpp; the sweep is
// host_node_sweep.cpp) and the nodes that are not elements — text, comments,
// fragments — with the tree surface every node shares (host_node.cpp).

#include "embed/embed.h"
#include "bronze_host/host_class.h"
#include "bronze_host/host_image.h"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace bro::dom {
class Element;
class Node;
class ShadowRoot;
}  // namespace bro::dom

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// ---------------------------------------------------------------------------
// The node registry (host_node_registry.cpp; the sweep is host_node_sweep.cpp)
// ---------------------------------------------------------------------------

// One entry per DOM node a wrapper, a record or a lookup has named, and the
// thing every accessor on a wrapper captures. Reached from a wrapper through
// embed::handleData, which is why `tag` is first (see the tag note in
// host_class.h). `node` is what the wrapper IS; `el` is the same pointer for
// an element and nullptr otherwise, so the element surface guards on `st->el`
// alone. Both go null when the node is freed (Document::addNodeFreedObserver).
// The entry is freed only once `jsRefs` — the JS objects that reach it
// natively: wrapper handles and the closures behind style, classList and
// dataset — is zero too.
struct SweepGroup;

// A listener's function and receiver, shared by the engine-side closure that
// calls it and the bookkeeping that finds it again, so the detached-tree sweep
// can take both out of the root set and put them back (host_node_sweep.cpp).
struct ListenerRef {
    ev::Persistent fn;
    ev::Persistent self;
};

struct HostNodeState {
    uint32_t tag = kHostElementTag;  // must be first — see the tag note in host_class.h
    dom::Node* node = nullptr;
    dom::Element* el = nullptr;
    dom::ShadowRoot* shadowRoot = nullptr;
    ev::Persistent jsObj;
    ev::Persistent styleObj;
    ev::Persistent classListObj;
    ev::Persistent computedObj;
    ev::Persistent datasetObj;
    std::unordered_map<std::string, uint64_t> inlineHandles;
    std::unordered_map<std::string, std::shared_ptr<ListenerRef>> inlineFns;
    uint32_t jsRefs = 0;
    bool pinned = false;  // a custom-element object stands in for the handle
    SweepGroup* sweepGroup = nullptr;  // demoted with this group (host_node_sweep.h)
    bool sweepAlone = false;           // tested in a group of its own
    bool hasStyle = false;
    bool hasClassList = false;
    bool hasComputed = false;
    bool hasDataset = false;
    bool fromImageConstructor = false;
    // <dialog> only (host_dialog.cpp): `returnValue`, and whether it was
    // opened by showModal() — the "is modal" flag show() and showModal()
    // check each other against.
    bool dialogModal = false;
    std::string dialogReturnValue;
    // Non-null only for an <img>: its src, its size and its RGBA. Owned here
    // so it dies with the node's entry.
    std::unique_ptr<HostImage> image;
};

// The entry for `node`, created on first ask. Never null for a non-null node.
HostNodeState* hostNodeStateFor(dom::Node* node);
// Whether the registry already has an entry for `node` (a wrapper, a record
// naming it). Never creates one. The DOM's node-retain query.
bool hostHasNodeState(const dom::Node* node);

// A fresh object that is already a node handle — what every wrapper in this
// layer is built on, so hostNodeOf() can recover the dom::Node* from a value
// the program hands back to appendChild.
Value makeNodeHandleObject(dom::Node* node);

// THE wrapper for `node`, whatever kind it is: an element through
// hostElementValue, a text or comment node through host_node.cpp, a fragment.
// Null for nullptr, so a parent/sibling lookup can be handed straight in.
Value hostNodeValue(dom::Node* node);

// The dom::Node behind any wrapper this layer made, or nullptr. The node-level
// counterpart of hostElementOf: appendChild takes this, because a text node is
// a legal child and is not an element.
dom::Node* hostNodeOf(Value v);

// The node state behind a host node VALUE (an element, text or comment
// wrapper). This is what a member on a shared prototype uses in place of a
// captured pointer: one copy of the method serves every node, so the receiver
// is the only thing that says which node the call is about.
HostNodeState* hostNodeStateOfValue(Value v);

// ---------------------------------------------------------------------------
// Text, comment and fragment nodes (host_node.cpp)
// ---------------------------------------------------------------------------

// The CharacterData surface — `data`, `nodeValue`, `textContent`, `length`,
// and the five mutators — over a TextNode or a CommentNode. They share every
// method and no base class, so the wrapper is written once against the pair.
Value makeCharacterDataValue(dom::Node* node);

// A DocumentFragment: a parent that holds children and vanishes into the tree
// when inserted. Nothing but the node surface, which is all a fragment has.
Value makeFragmentValue(dom::Node* frag);

// The NODE half of the tree surface — parentNode, childNodes, the child edges
// and siblings, the four mutators, contains, cloneNode, remove. Installed on
// every wrapper kind, because every one of them is a Node. The element-only
// extras (`children`, `firstElementChild`, querySelector…) stay in
// installElementCore beside it.
void installNodeTree(ObjectBuilder& b);

// Insert `child` under `parent` before `ref` (append when `ref` is null),
// unparenting it first and spilling a DocumentFragment's children in its place
// — the one insertion path all four mutators funnel through, so the fragment
// rule and the reparent rule are stated once.
void hostInsertNode(dom::Node* parent, dom::Node* child, dom::Node* ref);

}  // namespace bro::bronze_host
