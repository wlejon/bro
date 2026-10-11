#pragma once

#include "dom/element.h"

#include <cstdint>
#include <vector>

namespace bro::dom {

// Where `el` is in `root`'s composed tree (shadow trees and slots as the draw
// traversal walks them, Element::forEachComposedChild): `chain` from `el` up
// to `root` (both included) and `path`, each step's index among its composed
// parent's composed children, from `root` down. False when `el` is not
// rendered under `root`: in another tree, detached, or a light child no slot
// takes. Comparing paths orders elements in document order. For the few
// elements a document keeps a registry of (scroll containers, videos): it
// costs the siblings at each level.
inline bool composedPathFrom(const Element* root, Element* el,
                             std::vector<Element*>& chain, std::vector<uint32_t>& path) {
    chain.clear();
    path.clear();
    for (Element* e = el; e; e = e->layoutParent()) {
        chain.push_back(e);
        if (e == root) break;
    }
    if (chain.empty() || chain.back() != root) return false;
    for (size_t i = chain.size() - 1; i > 0; --i) {
        Element* parent = chain[i];
        Element* child = chain[i - 1];
        uint32_t index = 0;
        bool hit = false;
        parent->forEachComposedChild([&](Element* c) {
            if (hit) return;
            if (c == child) hit = true;
            else ++index;
        });
        if (!hit) return false;
        path.push_back(index);
    }
    return true;
}

}  // namespace bro::dom
