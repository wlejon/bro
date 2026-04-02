#pragma once

namespace bro::dom { class Element; class Node; }

namespace bro::engine {

/// Recursive hit test: find the deepest Element whose border box contains
/// the point (x, y). offsetX/offsetY accumulate from ancestor positioning.
/// Returns nullptr if no element is hit.
dom::Element* hitTestElement(dom::Element* elem, float x, float y,
                             float offsetX, float offsetY);

/// Hit test a node (delegates to hitTestElement for Element nodes).
dom::Element* hitTestNode(dom::Node* node, float x, float y,
                          float offsetX, float offsetY);

} // namespace bro::engine
