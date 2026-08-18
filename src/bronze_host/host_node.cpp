// Nodes that are not elements — text, comments, fragments — and the tree
// surface every node shares.
//
// host_element.cpp built a DOM out of elements only, which is exactly the tree
// an app that constructs its own UI sees: it calls createElement, appends, sets
// textContent, and never meets a node it did not make. The moment an app reads
// a tree it did NOT build — its own index.html, a template, anything parsed —
// the elements-only view starts lying. `panel.childNodes.length` counts three
// where the document holds seven, `firstChild` skips the whitespace HTML put
// there, and a UI that walks `nextSibling` to find its label walks past it.
//
// Three things here are worth stating rather than reading off.
//
// TEXT IS A NODE, NOT A STRING. `textContent` on an element has always worked;
// it flattens. What did not exist was a VALUE for the text node itself, so
// there was nothing for childNodes to hand back, nothing to appendChild, and no
// way to change one word of a paragraph without rewriting all of it. Every text
// node now gets a wrapper out of the same registry elements come from, so
// `p.firstChild === textNode` holds for the same reason it holds for elements.
//
// CHARACTERDATA IS WRITTEN TWICE IN THE DOM AND ONCE HERE. dom::TextNode and
// dom::CommentNode carry identical `data`/`appendData`/`splitText`-shaped APIs
// and share no base class, so a wrapper written against either one would have
// to be written against both. The `chars*` helpers below switch on nodeType
// once and the surface is defined once on top of them.
//
// A FRAGMENT IS A PARENT THAT DISAPPEARS. `createDocumentFragment` exists to
// batch: build ten rows off-tree, insert once, lay out once. That only works if
// insertion SPILLS the fragment's children in its place instead of inserting
// the fragment itself, which is why all four mutators funnel through
// hostInsertNode rather than calling dom::Node::appendChild directly. An app
// that appends a fragment and finds one node in the tree instead of ten has hit
// the version of this that forgot.

#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include "dom/comment_node.h"
#include "dom/document.h"
#include "dom/document_fragment.h"
#include "dom/element.h"
#include "dom/node.h"
#include "dom/text_node.h"
#include "dom/text_offsets.h"

#include <algorithm>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// ---------------------------------------------------------------------------
// CharacterData over the two node types that have it
// ---------------------------------------------------------------------------

bool isCharacterData(const dom::Node* n) {
    return n && (n->nodeType() == dom::NodeType::Text ||
                 n->nodeType() == dom::NodeType::Comment);
}

std::string charsData(dom::Node* n) {
    if (!n) return {};
    if (n->nodeType() == dom::NodeType::Text)
        return static_cast<dom::TextNode*>(n)->data();
    if (n->nodeType() == dom::NodeType::Comment)
        return static_cast<dom::CommentNode*>(n)->data();
    return {};
}

void charsSetData(dom::Node* n, const std::string& text) {
    if (!n) return;
    if (n->nodeType() == dom::NodeType::Text)
        static_cast<dom::TextNode*>(n)->setData(text);
    else if (n->nodeType() == dom::NodeType::Comment)
        static_cast<dom::CommentNode*>(n)->setData(text);
}

// OFFSETS ARE UTF-16, STORAGE IS UTF-8. Every index in the CharacterData API
// is a JS string index — `data.length`, the offset appendData counts from, what
// splitText cuts at — and JS strings are UTF-16. dom::TextNode stores bytes. So
// an offset from compiled code is converted before it reaches the DOM, and a
// length reported back is converted the other way. Skipping this is invisible
// in ASCII and cuts a multi-byte character in half the first time an app
// touches a name with an accent in it.
//
// Out-of-range is clamped rather than thrown: the web raises IndexSizeError and
// this layer has no DOMException to raise, so clamping is the honest reading of
// "do what was asked as far as it makes sense".
int clampU16(double v, int len) {
    if (!(v > 0)) return 0;  // also catches NaN
    return v >= static_cast<double>(len) ? len : static_cast<int>(v);
}

// A byte offset and byte count for the [offset, offset+count) UTF-16 range,
// both clamped into `s`.
struct ByteRange { size_t offset; size_t count; };

ByteRange byteRangeOf(const std::string& s, double offU16, double countU16) {
    int len = dom::utf16Length(s);
    int off = clampU16(offU16, len);
    int end = off + clampU16(countU16, len - off);
    size_t byteOff = static_cast<size_t>(dom::utf16ToUtf8Byte(s, off));
    size_t byteEnd = static_cast<size_t>(dom::utf16ToUtf8Byte(s, end));
    return {byteOff, byteEnd - byteOff};
}

void charsInsert(dom::Node* n, size_t offset, const std::string& s) {
    if (n->nodeType() == dom::NodeType::Text)
        static_cast<dom::TextNode*>(n)->insertData(offset, s);
    else
        static_cast<dom::CommentNode*>(n)->insertData(offset, s);
}

void charsDelete(dom::Node* n, size_t offset, size_t count) {
    if (n->nodeType() == dom::NodeType::Text)
        static_cast<dom::TextNode*>(n)->deleteData(offset, count);
    else
        static_cast<dom::CommentNode*>(n)->deleteData(offset, count);
}

void charsReplace(dom::Node* n, size_t offset, size_t count, const std::string& s) {
    if (n->nodeType() == dom::NodeType::Text)
        static_cast<dom::TextNode*>(n)->replaceData(offset, count, s);
    else
        static_cast<dom::CommentNode*>(n)->replaceData(offset, count, s);
}

std::string charsSubstring(dom::Node* n, size_t offset, size_t count) {
    if (n->nodeType() == dom::NodeType::Text)
        return static_cast<dom::TextNode*>(n)->substringData(offset, count);
    return static_cast<dom::CommentNode*>(n)->substringData(offset, count);
}

// ---------------------------------------------------------------------------
// Tree helpers
// ---------------------------------------------------------------------------

// The child list, copied. Every caller either hands it to a mutator or builds
// an array from it, and both allocate — so a reference into the node's own
// vector would dangle the moment the tree changed under it.
std::vector<dom::Node*> childrenOf(dom::Node* n) {
    if (!n) return {};
    return n->childNodes();
}

// `n` is `other` or an ancestor of it. The web's contains() answers true for
// the node itself, which is what makes `panel.contains(event.target)` the
// standard "was this click mine" test.
bool nodeContains(dom::Node* n, dom::Node* other) {
    for (dom::Node* p = other; p; p = p->parentNode())
        if (p == n) return true;
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Insertion, in one place
// ---------------------------------------------------------------------------

void hostInsertNode(dom::Node* parent, dom::Node* child, dom::Node* ref) {
    if (!parent || !child || parent == child) return;
    // Inserting an ancestor under its own descendant would make a cycle, and
    // the tree walks below (parentNode chains, layout traversal) do not
    // terminate on one. The web throws HierarchyRequestError; refusing is the
    // same protection without a DOMException to throw it with.
    if (nodeContains(child, parent)) return;

    if (child->nodeType() == dom::NodeType::DocumentFragment) {
        // The fragment stays behind, emptied. Its children are what enters the
        // tree — one at a time and in order, each unparented from the fragment
        // by the insert itself.
        for (dom::Node* kid : childrenOf(child)) {
            child->removeChild(kid);
            parent->insertBefore(kid, ref);
        }
        return;
    }
    if (child->parentNode()) child->parentNode()->removeChild(child);
    parent->insertBefore(child, ref);
}

// ---------------------------------------------------------------------------
// The tree surface every wrapper gets
// ---------------------------------------------------------------------------

// Every member here reads its RECEIVER rather than closing over the state, so
// the same call serves an element prototype (one copy for every element in the
// document) and a text or comment wrapper that still carries its own.
void installNodeTree(ObjectBuilder& b) {
    b.def("appendChild", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        dom::Node* child = hostNodeOf(argAt(a, 0));
        if (!st->node || !child)
            return ev::throwTypeError("appendChild: argument is not a node");
        hostInsertNode(st->node, child, nullptr);
        return argAt(a, 0);
    });
    b.def("removeChild", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        dom::Node* child = hostNodeOf(argAt(a, 0));
        if (!st->node || !child)
            return ev::throwTypeError("removeChild: argument is not a node");
        if (child->parentNode() == st->node) st->node->removeChild(child);
        return argAt(a, 0);
    });
    b.def("insertBefore", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        dom::Node* child = hostNodeOf(argAt(a, 0));
        if (!st->node || !child)
            return ev::throwTypeError("insertBefore: argument is not a node");
        // A ref node that is not our child is ignored rather than an error, so
        // insertBefore(x, null) — the spelling of "append" every UI library
        // uses — keeps working.
        dom::Node* ref = hostNodeOf(argAt(a, 1));
        if (ref && ref->parentNode() != st->node) ref = nullptr;
        hostInsertNode(st->node, child, ref);
        return argAt(a, 0);
    });
    b.def("replaceChild", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        dom::Node* fresh = hostNodeOf(argAt(a, 0));
        dom::Node* old = hostNodeOf(argAt(a, 1));
        if (!st->node || !fresh || !old)
            return ev::throwTypeError("replaceChild: argument is not a node");
        if (old->parentNode() != st->node) return argAt(a, 1);
        hostInsertNode(st->node, fresh, old);
        st->node->removeChild(old);
        return argAt(a, 1);
    });
    b.def("remove", 0, [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        if (st->node && st->node->parentNode())
            st->node->parentNode()->removeChild(st->node);
        return ev::undefined();
    });
    // The real answer, not the constant `false` the earliest wrapper returned:
    // a UI asks `panel.contains(event.target)` to decide whether a click was
    // its own, and a wrong answer there closes menus that should have stayed
    // open.
    b.def("contains", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        dom::Node* other = hostNodeOf(argAt(a, 0));
        if (!st->node || !other) return ev::fromBool(false);
        return ev::fromBool(nodeContains(st->node, other));
    });

    // cloneNode goes through the DOCUMENT's clone, not a rebuild from this
    // layer: dom::Document::cloneNode is the spec algorithm over the real node
    // storage, so a cloned <canvas> keeps its backing store and a cloned
    // <select> its live selection — neither of which survives the obvious
    // implementation of walking the tree and re-creating elements from their
    // tag names and attributes.
    b.def("cloneNode", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        if (!st->node) return ev::null();
        dom::Document* doc = st->node->document();
        if (!doc) return ev::null();
        bool deep = ev::toBool(argAt(a, 0));
        // preserveId=false matches what bro's own JS binding does: two nodes
        // with one id is a bug an app almost never intends, and getElementById
        // would answer whichever it reached first.
        return hostNodeValue(doc->cloneNode(st->node, deep, /*preserveId=*/false));
    });

    b.accessor("childNodes",
               [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
                   std::vector<dom::Node*> kids = childrenOf(st->node);
                   return hostArrayOf(kids.size(), [&kids](size_t i) {
                       return hostNodeValue(kids[i]);
                   });
               },
               nullptr);
    b.accessor("hasChildNodes",
               [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
                   return ev::fromBool(st->node && !st->node->childNodes().empty());
               },
               nullptr);

    b.accessor("parentNode",
               [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
                   return st->node ? hostNodeValue(st->node->parentNode())
                                   : ev::null();
               },
               nullptr);

    auto defEdge = [&b](const char* name, bool first) {
        b.accessor(name,
                   [first](Value self_, std::span<const Value>) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       if (!st || !st->node) return ev::null();
                       const std::vector<dom::Node*>& kids = st->node->childNodes();
                       if (kids.empty()) return ev::null();
                       return hostNodeValue(first ? kids.front() : kids.back());
                   },
                   nullptr);
    };
    defEdge("firstChild", true);
    defEdge("lastChild", false);

    auto defSibling = [&b](const char* name, int dir) {
        b.accessor(name,
                   [dir](Value self_, std::span<const Value>) {
                       HostNodeState* st = hostNodeStateOfValue(self_);
                       if (!st || !st->node) return ev::null();
                       dom::Node* parent = st->node->parentNode();
                       if (!parent) return ev::null();
                       const std::vector<dom::Node*>& kids = parent->childNodes();
                       auto it = std::find(kids.begin(), kids.end(), st->node);
                       if (it == kids.end()) return ev::null();
                       long long want =
                           static_cast<long long>(it - kids.begin()) + dir;
                       if (want < 0 || want >= static_cast<long long>(kids.size()))
                           return ev::null();
                       return hostNodeValue(kids[static_cast<size_t>(want)]);
                   },
                   nullptr);
    };
    defSibling("nextSibling", +1);
    defSibling("previousSibling", -1);
}

// ---------------------------------------------------------------------------
// The wrappers
// ---------------------------------------------------------------------------

Value makeCharacterDataValue(dom::Node* node) {
    if (!isCharacterData(node)) return ev::null();
    HostNodeState* st = hostNodeStateFor(node);
    ObjectBuilder b(makeNodeHandleObject(node));

    b.set("nodeType",
          ev::fromDouble(node->nodeType() == dom::NodeType::Text ? 3.0 : 8.0));
    b.set("nodeName", ev::fromUtf8(node->nodeName()));

    // data / nodeValue / textContent are three names for one string on a
    // character-data node, and libraries reach for all three.
    auto defData = [&b, st](const char* name) {
        b.accessor(name,
                   [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
                       return ev::fromUtf8(charsData(st->node));
                   },
                   [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
                       Value v = argAt(a, 0);
                       if (st->node && !ev::isObject(v))
                           charsSetData(st->node,
                                        ev::isUndefined(v) ? "" : ev::toUtf8(v));
                       return ev::undefined();
                   });
    };
    defData("data");
    defData("nodeValue");
    defData("textContent");

    b.accessor("length",
               [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
                   return ev::fromDouble(dom::utf16Length(charsData(st->node)));
               },
               nullptr);

    b.def("appendData", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        if (st->node)
            charsSetData(st->node, charsData(st->node) + ev::toUtf8(argAt(a, 0)));
        return ev::undefined();
    });
    b.def("insertData", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        if (!st->node) return ev::undefined();
        ByteRange r = byteRangeOf(charsData(st->node), ev::toDouble(argAt(a, 0)), 0);
        charsInsert(st->node, r.offset, ev::toUtf8(argAt(a, 1)));
        return ev::undefined();
    });
    b.def("deleteData", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        if (!st->node) return ev::undefined();
        ByteRange r = byteRangeOf(charsData(st->node), ev::toDouble(argAt(a, 0)),
                                  ev::toDouble(argAt(a, 1)));
        charsDelete(st->node, r.offset, r.count);
        return ev::undefined();
    });
    b.def("replaceData", 3, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        if (!st->node) return ev::undefined();
        ByteRange r = byteRangeOf(charsData(st->node), ev::toDouble(argAt(a, 0)),
                                  ev::toDouble(argAt(a, 1)));
        charsReplace(st->node, r.offset, r.count, ev::toUtf8(argAt(a, 2)));
        return ev::undefined();
    });
    b.def("substringData", 2, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        if (!st->node) return ev::fromUtf8("");
        ByteRange r = byteRangeOf(charsData(st->node), ev::toDouble(argAt(a, 0)),
                                  ev::toDouble(argAt(a, 1)));
        return ev::fromUtf8(charsSubstring(st->node, r.offset, r.count));
    });

    // splitText is Text-only, and it is done here rather than through
    // dom::TextNode::splitText because that one hands back a bare `new TextNode`
    // the caller must take ownership of. A node this layer allocated would be
    // outside Document::ownedNodes_ — so it would never be freed, and the
    // freed-node observer the registry depends on would never fire for it. The
    // document's own factory is the only allocation path with those properties.
    if (node->nodeType() == dom::NodeType::Text) {
        b.def("splitText", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
            if (!st->node) return ev::null();
            auto* text = static_cast<dom::TextNode*>(st->node);
            dom::Document* doc = text->document();
            if (!doc) return ev::null();

            const std::string& data = text->data();
            size_t cut = static_cast<size_t>(dom::utf16ToUtf8Byte(
                data, clampU16(ev::toDouble(argAt(a, 0)), dom::utf16Length(data))));
            std::string tail = data.substr(cut);
            text->setData(data.substr(0, cut));

            dom::TextNode* fresh = doc->createTextNode(tail);
            // Insert immediately after this node, through insertBefore so the
            // parent's layout is invalidated the way any other tree edit does
            // it (dom/node.h).
            if (dom::Node* parent = st->node->parentNode()) {
                const std::vector<dom::Node*>& kids = parent->childNodes();
                auto it = std::find(kids.begin(), kids.end(), st->node);
                dom::Node* after =
                    (it != kids.end() && it + 1 != kids.end()) ? *(it + 1) : nullptr;
                parent->insertBefore(fresh, after);
            }
            return hostNodeValue(fresh);
        });
    }

    installNodeTree(b);
    return b.get();
}

Value makeFragmentValue(dom::DocumentFragment* frag) {
    if (!frag) return ev::null();
    HostNodeState* st = hostNodeStateFor(frag);
    ObjectBuilder b(makeNodeHandleObject(frag));

    b.set("nodeType", ev::fromDouble(11));
    b.set("nodeName", ev::fromUtf8("#document-fragment"));

    // A fragment's children may be elements, so it gets the element-flavoured
    // views too — a UI builds rows into a fragment and then reads
    // `frag.children.length` back before inserting it.
    b.accessor("children",
               [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
                   std::vector<dom::Element*> kids;
                   if (st->node)
                       for (dom::Node* n : st->node->childNodes())
                           if (n->nodeType() == dom::NodeType::Element)
                               kids.push_back(static_cast<dom::Element*>(n));
                   return hostArrayOf(kids.size(), [&kids](size_t i) {
                       return hostElementValue(kids[i]);
                   });
               },
               nullptr);

    b.def("append", 1, [](Value self_, std::span<const Value> a) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
        if (!st->node) return ev::undefined();
        for (const Value& v : a) {
            if (dom::Node* child = hostNodeOf(v)) {
                hostInsertNode(st->node, child, nullptr);
            } else if (!ev::isObject(v) && !ev::isUndefined(v)) {
                if (dom::Document* doc = st->node->document())
                    st->node->appendChild(doc->createTextNode(ev::toUtf8(v)));
            }
        }
        return ev::undefined();
    });

    b.accessor("textContent",
               [](Value self_, std::span<const Value>) {
        HostNodeState* st = hostNodeStateOfValue(self_);
        if (!st) return ev::undefined();
                   std::string out;
                   if (st->node)
                       for (dom::Node* n : st->node->childNodes())
                           if (n->nodeType() == dom::NodeType::Text)
                               out += static_cast<dom::TextNode*>(n)->data();
                   return ev::fromUtf8(out);
               },
               nullptr);

    installNodeTree(b);
    return b.get();
}

}  // namespace bro::bronze_host
