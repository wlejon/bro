#pragma once

// Input helpers shared between the app-document input pipeline
// (input_handling.cpp) and the per-window host pipeline
// (window_host_input.cpp).
//
// These were file-static in input_handling.cpp until secondary windows grew
// their own input path. They are pure translations (SDL conventions → DOM
// conventions, CSS keyword → platform cursor) with no engine state, so both
// pipelines can share one definition instead of drifting apart.

#include "dom/event.h"
#include "platform/sdl_window.h"

#include <string>

namespace bro::dom { class Element; }

namespace bro::engine {

/// SDL text input start/stop against a window — no-ops without one. `window`
/// selects WHICH OS window the IME targets, which is the whole point in a
/// multi-window app: the focused window's text control owns the candidate box.
void safeStartTextInput(platform::Window* window);
void safeStopTextInput(platform::Window* window);

/// The SDL_KMOD_* bit a modifier keycode maps to (both left/right variants
/// fold onto the same bit, matching SDL_GetModState()). 0 for non-modifiers.
int modifierBitForKeycode(int keycode);

/// A control that owns a text caret and selection: a <textarea>, or an <input>
/// of a text-ish type. Excludes checkbox/radio/range/color/button inputs, where
/// a press means something else entirely and a drag is not a text drag.
bool isCaretControl(dom::Element* el);

/// Build a KeyboardEvent with key/code/modifier/location fields populated.
dom::KeyboardEvent makeKeyboardEvent(const char* type, int keycode, int scancode,
                                     int mod, bool repeat);

/// Convert an SDL3 mouse button id (1=left, 2=middle, 3=right, 4=X1, 5=X2)
/// into the DOM MouseEvent.button index (0=left, 1=middle, 2=right, 3=back,
/// 4=forward). SDL and the DOM disagree on both the base index and the
/// middle/right ordering.
int sdlToDomButton(int sdlButton);

/// MouseEvent.buttons bitmask value for a DOM button index (DOM swaps right
/// and middle relative to a naive 1<<n encoding).
int domButtonMask(int domButton);

/// Populate a MouseEvent's standard fields. `x, y` are the window-space
/// position; `contentTop` is the engine-reserved top inset (menu bar) so
/// clientY/pageY come out in the web-standard content space. Secondary
/// windows have no engine chrome and pass 0 for both contentTop and scrollY.
void populateMouseEvent(dom::MouseEvent& evt, float x, float y,
                        int button, int buttons,
                        float movementX, float movementY,
                        float scrollY, int mod,
                        float contentTop = 0.0f);

/// Collapse a computed CSS `cursor` value onto a platform CursorShape, and
/// the stable name for one (the string resolvedCursor() reports).
platform::CursorShape cursorShapeFromCss(const std::string& value);
const char* cursorShapeName(platform::CursorShape s);

/// Control characters (tab, DEL, ...) that must never be inserted as text.
bool isControlChar(const std::string& text);

} // namespace bro::engine
