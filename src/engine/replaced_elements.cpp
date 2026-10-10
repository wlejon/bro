// Shared replaced-element initialization and interaction logic.
// Used by the Engine for both the app document and system panels.

#include "engine/replaced_elements.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "engine/color_picker_overlay.h"
#include "engine/dropdown_overlay.h"
#include "engine/overlay.h"
#include "layout/el_select.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_svg.h"
#include "layout/el_video.h"
#include "layout/el_terminal.h"
#include "layout/el_remote_view.h"
#include "layout/image_loading.h"
#include "platform/window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace bro::engine {


// ---------------------------------------------------------------------------
// Replaced element initialization
// ---------------------------------------------------------------------------

void ensureReplacedElements(dom::Element* elem, render::Renderer* renderer,
                            broaudio::Engine* audioEngine) {
    if (!elem) return;

    const auto& tag = elem->tagName();

    if (tag == "INPUT" && !elem->inputControl()) {
        auto ctrl = std::make_unique<layout::ElInput>(renderer);
        ctrl->setElement(elem);
        elem->setInputControl(std::move(ctrl));
    } else if (tag == "TEXTAREA" && !elem->textareaControl()) {
        auto ctrl = std::make_unique<layout::ElTextarea>(renderer);
        ctrl->setElement(elem);
        elem->setTextareaControl(std::move(ctrl));
    } else if (tag == "SELECT" && !elem->selectControl()) {
        auto ctrl = std::make_unique<layout::ElSelect>(renderer);
        ctrl->setElement(elem);
        ctrl->initSelectedIndex();
        elem->setSelectControl(std::move(ctrl));
    } else if ((tag == "SVG" || tag == "svg") && !elem->svgControl()) {
        auto ctrl = std::make_unique<layout::ElSvg>(renderer);
        ctrl->setElement(elem);
        ctrl->parseAttributes();
        elem->setSvgControl(std::move(ctrl));
    } else if (tag == "IMG" || tag == "img") {
        // Start the image's load (layout/image_loading.h): a src set in
        // markup, through setAttribute or innerHTML. The decode runs off the
        // page thread; layout gets the intrinsic size from the header now
        // where that is cheap — without one an <img> is not a replaced
        // element and lays out as an empty inline box — and the load / error
        // event follows as a task when the picture lands.
        //
        // Only when `src` changes: this runs on every DOM-dirty pass.
        const std::string src = elem->getAttribute("src");
        if (!src.empty() && src != elem->imageProbedSrc()) {
            layout::loadImageElement(elem, src);
        } else if (src.empty() && !elem->imageProbedSrc().empty()) {
            // src removed: drop the stale size rather than keep sizing the
            // box from an image that is no longer referenced.
            layout::loadImageElement(elem, "");
        }
    } else if ((tag == "VIDEO" || tag == "video") && !elem->videoControl()) {
        auto ctrl = std::make_unique<layout::ElVideo>(renderer);
        ctrl->setElement(elem);
        ctrl->setAudioEngine(audioEngine);
        // If the element already has a src attribute, load it now.
        // Otherwise the binding will trigger load when src is set.
        std::string src = elem->getAttribute("src");
        if (!src.empty()) ctrl->load(src);
        elem->setVideoControl(std::move(ctrl));
    } else if ((tag == "TERMINAL" || tag == "terminal") && !elem->terminalControl()) {
        auto ctrl = std::make_unique<layout::ElTerminal>(renderer);
        ctrl->setElement(elem);
        elem->setTerminalControl(std::move(ctrl));
    } else if ((tag == "REMOTEVIEW" || tag == "remoteview") && !elem->remoteViewControl()) {
        auto ctrl = std::make_unique<layout::ElRemoteView>();
        ctrl->setElement(elem);
        elem->setRemoteViewControl(std::move(ctrl));
    }

    // Recurse into children
    for (auto* child : elem->childNodes()) {
        if (child->nodeType() == dom::NodeType::Element) {
            ensureReplacedElements(static_cast<dom::Element*>(child), renderer,
                                   audioEngine);
        }
    }

    // Recurse into shadow DOM
    if (elem->hasShadow()) {
        auto* sr = elem->shadowRoot();
        for (auto* child : sr->childNodes()) {
            if (child->nodeType() == dom::NodeType::Element) {
                ensureReplacedElements(static_cast<dom::Element*>(child),
                                       renderer, audioEngine);
            }
        }
    }
}

} // namespace bro::engine

