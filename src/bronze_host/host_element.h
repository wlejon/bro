#pragma once

// Elements (host_element.cpp), and the parts of the element surface split out
// of it: style, dataset and classList, forms, validity, mutation, interaction
// and geometry (host_element_*.cpp).

#include "embed/embed.h"

#include <string>
#include <vector>

namespace bro::dom {
class Document;
class Element;
struct AbsoluteRect;
}  // namespace bro::dom

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

class HostClass;
struct HostNodeState;
// host_builder.h owns it; a file that only names it needs nothing more.
struct ObjectBuilder;

// THE element wrapper for `el` — built on first ask, the same value every time
// after that, because identity is what a UI tests (`event.target === this.dom`).
// Answers null for nullptr, so it can be handed a parent/sibling lookup result
// directly.
Value hostElementValue(dom::Element* el);

// The dom::Element behind a wrapper, or nullptr for any other value. This is
// what makes `parent.appendChild(child)` possible: the wrapper is an embed
// handle whose data is its registry entry.
dom::Element* hostElementOf(Value v);

// getComputedStyle(el): the LIVE resolved-value declaration for an element
// wrapper — used widths off the layout box, lengths in px, colours as rgb() —
// resolved by layout::computedProperty, the same function bro's own JS
// bindings answer from. Anything that is not an element wrapper answers an
// object whose properties are all the empty string, which is what those
// bindings do too.
Value hostComputedStyleFor(Value elValue);

// Record a wrapper this file did not build — dom_globals.cpp's canvas, which
// is an element plus a drawing buffer — so it keeps its identity in the
// registry like any other.
void noteHostElementValue(dom::Element* el, Value v);

bool isCanvasTag(const std::string& tag);
bool isImgTag(const std::string& tag);

// The class every element is born on. Exposed so that a SUBCLASS — `Image`,
// today the only one — can chain its prototype onto it.
const HostClass& elementHostClass();

// A fresh object that is ALREADY an element handle — what every element
// wrapper in this layer must be built on, so hostElementOf() can recover the
// dom::Element* from a value the program hands back to appendChild.
Value makeElementHandleObject(dom::Element* el);

// The whole element surface — identity, style, classList, attributes, the
// tree, geometry, focus, and the event target — onto an object under
// construction. Shared, so a canvas is an element that also has a drawing
// buffer rather than a separate kind of thing that happens to look like one.
void installElementCore(ObjectBuilder& b, dom::Element* el);

// Installs Element/HTMLElement as a real class. Must run BEFORE any element
// value is built, or those elements are born on the bare handle shape and
// carry no members at all.
void installElementGlobals();

// An element and nothing more (host_element.cpp); a canvas (dom_globals.cpp).
Value makePlainElementValue(dom::Element* el);
Value makeCanvasElementValue(dom::Element* el);

// Fullscreen element tracking (host_element.cpp owns it).
void setHostFullscreenElement(dom::Element* el);
dom::Element* hostFullscreenElement();

// The host object this layer handed the program for `el` — the canvas value
// from document.createElement('canvas') — or undefined for an element it
// never wrapped. This is what makes `event.target === canvas` true inside a
// compiled listener, and it is identity, not a rebuild: the same Value the
// program already holds.
Value hostValueForElement(dom::Element* el);
Value describeTarget(dom::Element* el);

// ---------------------------------------------------------------------------
// Style & dataset decomposition (host_element_style.cpp / dataset.cpp / forms.cpp)
// ---------------------------------------------------------------------------
Value makeStyleObject(HostNodeState* st);
Value makeComputedStyleObject(HostNodeState* st);
void decorateElementStyle(ObjectBuilder& b);

Value makeDatasetObject(HostNodeState* st);
Value makeClassListObject(HostNodeState* st);
void decorateElementDataset(ObjectBuilder& b);

void decorateElementForms(ObjectBuilder& b);
void decorateElementValidity(ObjectBuilder& b);
void decorateElementMutate(ObjectBuilder& b);
void decorateElementInteraction(ObjectBuilder& b);
void decorateElementGeometry(ObjectBuilder& b);
dom::AbsoluteRect borderBoxOf(dom::Element* el);
// How far client coordinates sit above `el`'s document coordinates: the app
// document's root scroller (viewport) offset, 0 in any other document (an
// iframe, a secondary window, a system panel). borderBoxOf / clientRectsOf
// are document coordinates; a CSSOM client rect subtracts this.
float viewportScrollOf(const dom::Element* el);
// The root scroller of the app document is the viewport, unless <html> is a
// scroll container of its own with somewhere to scroll (an app that sizes it
// to the window and lets it overflow). `el` is the element that plays that
// part when it is <html>; null means the viewport.
dom::Element* rootScrollerElement();
// window.scrollY: the root scroller's offset.
float rootScrollY();
// window.scrollTo(_, y): move the root scroller.
void scrollRootTo(float y);
// Element.getClientRects(): one border box per line fragment for an inline
// element, the border box otherwise, none for display:none/contents. Same
// coordinate space as borderBoxOf.
std::vector<dom::AbsoluteRect> clientRectsOf(dom::Element* el);

// Install the constraint-validation pattern tester (layout::form_validation)
// on top of this realm's RegExp. Called once, when the host globals go in.
void installHostPatternTester();
Value makeLiveHTMLCollection(dom::Element* root, dom::Document* fixed, std::string selector);

}  // namespace bro::bronze_host
