#pragma once

#include <string>

namespace bro::layout {

/// Result returned by control key/text handlers.
/// The engine uses these flags to dispatch appropriate DOM events.
struct KeyHandleResult {
    bool handled = false;         // Control consumed the key
    bool dispatchInput = false;   // Fire an InputEvent
    std::string inputData;        // InputEvent.data
    std::string inputType;        // InputEvent.inputType
    bool dispatchChange = false;  // Fire a change event
    bool unfocus = false;         // Unfocus the control
};

} // namespace bro::layout
