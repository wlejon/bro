#pragma once

// The DOM's list interfaces (host_dom_lists.cpp): NodeList (DOM §4.2.10.1),
// HTMLCollection (§4.2.10.2), and HTML's HTMLOptionsCollection and
// HTMLFormControlsCollection, as interface objects a page can name
// (`x instanceof NodeList`, `NodeList.prototype.forEach`) and the list
// objects the DOM answers with.
//
// A static list (querySelectorAll) is an ordinary object born on
// NodeList.prototype holding its nodes as indexed properties. A live list is a
// host proxy (host_proxy.h) on its interface's prototype whose length and
// indices are read from the tree at each access: childNodes and children are
// one such object per node (`el.children === el.children`), cached on the
// node's entry.

#include "bronze_host/host_class.h"

#include <functional>
#include <vector>

namespace bro::dom {
class Node;
class Element;
}  // namespace bro::dom

namespace bro::bronze_host {

struct HostNodeState;

// NodeList, HTMLCollection, HTMLOptionsCollection, HTMLFormControlsCollection:
// constructors that throw (the web's "Illegal constructor") and the
// prototypes the list objects are born on. Once per realm, with the HTML
// interfaces.
void installDomListInterfaces();

// The prototypes of DOMTokenList (classList, relList), DOMStringMap (dataset)
// and MediaQueryList (matchMedia), installed with the lists; their members are
// the objects' own (host_element_dataset.cpp, host_matchmedia.cpp).
Value domTokenListProto();
Value domStringMapProto();
Value mediaQueryListProto();

// querySelectorAll's: the nodes as they were when it ran.
Value makeStaticNodeList(const std::vector<dom::Element*>& nodes);

// A live NodeList over what `items` answers at each read.
Value makeLiveNodeList(std::function<std::vector<dom::Node*>()> items);

enum class CollectionKind { HTMLCollection, Options, FormControls };

// A live HTMLCollection (or one of its HTML subclasses) over what `items`
// answers at each read: indices, then the named properties (an element's id,
// or its name attribute) that the prototype's members do not shadow.
// HTMLOptionsCollection's `length` is writable: a smaller length removes the
// options past it (`select.options.length = 0` empties a select).
Value makeLiveCollection(std::function<std::vector<dom::Element*>()> items,
                         CollectionKind kind = CollectionKind::HTMLCollection);

// node.childNodes and parentNode.children: live, the same object every time.
Value hostChildNodesList(HostNodeState* st);
Value hostChildrenCollection(HostNodeState* st);

}  // namespace bro::bronze_host
