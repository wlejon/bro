#pragma once

namespace bro::bronze_host {

/// Dispatches focus / blur event to bro.window JS listeners.
void dispatchWindowFocus(bool gained);

} // namespace bro::bronze_host
