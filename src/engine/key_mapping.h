#pragma once

#include <cstdint>
#include <string>

namespace bro::engine {

/// Map an SDL keycode + modifier state to a standard web KeyboardEvent.key value.
std::string sdlKeycodeToWebKey(int32_t keycode, int mod);

/// Map an SDL scancode to a standard web KeyboardEvent.code value.
std::string sdlScancodeToWebCode(int32_t scancode);

} // namespace bro::engine
