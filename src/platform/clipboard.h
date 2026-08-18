#pragma once

#include <string>

namespace bro::platform {

// The system clipboard, with the one thing SDL leaves to the caller: a retry.
//
// **The clipboard is opened exclusively, one process at a time.** On Windows
// `OpenClipboard` fails outright while somebody else holds it — a clipboard
// manager polling it, an editor, another bro — and SDL reports that faithfully
// and gives up. Measured with six bro processes contending: 259 failed writes
// in 300, and one process that failed all 300. The same loop alone: 300 for
// 300. So this is not a rare corner; it is what a copy does on a desk with a
// clipboard manager running.
//
// The contention is transient by nature — a holder opens, reads and closes in
// microseconds — so a few attempts spread over a few milliseconds turn "copy
// silently does nothing" into "copy works". Bounded deliberately: this runs on
// the thread that draws, and a copy that cannot be made in about ten
// milliseconds is better reported to the caller than waited on.
//
// Callers must honour the result. A `copy` that reports success it did not
// have is a paste of somebody else's text later; a `cut` that does is the
// text deleted and nowhere to paste it back from.
bool setClipboardText(const std::string& text);

// The reading half. `SDL_GetClipboardText` answers "" both for a clipboard
// that is empty and for one it could not open, so the retry is also the only
// way to tell those apart. An empty clipboard is a success with an empty
// string; only a read that never got in is a failure.
std::string getClipboardText(bool* ok = nullptr);

} // namespace bro::platform
