#pragma once

namespace bro::dom { class Element; }

namespace bro::engine {

enum class InspectorDock { Right, Bottom };

struct InspectorState {
    bool visible = false;
    InspectorDock dock = InspectorDock::Right;
    int width = 320;
    int height = 280;
    bool pickerMode = false;

    // Non-owning. Validated against the live document tree before each use.
    dom::Element* selected = nullptr;
    dom::Element* pickerHover = nullptr;
};

} // namespace bro::engine
