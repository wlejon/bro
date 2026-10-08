#pragma once

// Shared by the engine's control commands (control_commands.cpp,
// control_input.cpp, control_record.cpp).

#include <cstdint>
#include <string>
#include <vector>

namespace bro::dom { class Element; }

namespace bro::engine {

class Engine;

/// A rect in CSS px, client coordinates (what getBoundingClientRect says).
struct ControlRect {
    float x = 0, y = 0, width = 0, height = 0;
};

/// "<div#id.a.b>" for a log line or a reply.
std::string describeElement(const dom::Element* el);

/// The client rects of every element matching `selector` in the app
/// document, laid out first. Empty with `why` on a bad selector or none.
std::vector<std::pair<dom::Element*, ControlRect>> controlQueryRects(Engine& engine, const std::string& selector,
                                                                     std::string* why);

/// Where an input command aims: "x,y", or two numbers, in CSS px; or a
/// selector, which aims at the centre of the first match that has a box.
/// `args` are the command's positional arguments from `index`; `used` says
/// how many the target took.
bool controlResolvePoint(Engine& engine, const std::vector<std::string>& args, size_t index, float& x, float& y,
                         size_t* used, std::string* why);

/// What is on screen as RGBA8: the scanout buffer under DRM, else the
/// engine's own composite. Engine thread.
bool controlGrabScreen(Engine& engine, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height,
                       std::string* why);

/// $XDG_RUNTIME_DIR/bro-control/<file>.
std::string controlRuntimePath(const std::string& file);

}  // namespace bro::engine
