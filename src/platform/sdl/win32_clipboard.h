#pragma once
// The Windows clipboard's multi-format write and image reads, in Win32
// (win32_clipboard.cpp). Windows only; sdl_clipboard.cpp is the caller.

#include "platform/clipboard.h"

#include <optional>
#include <string>
#include <vector>

namespace bro::platform::win32clip {

enum class Result { Ok, Busy, Failed };

/// Replace the clipboard with these representations: text as
/// CF_UNICODETEXT, a PNG as "PNG" + CF_DIBV5 + CF_DIB, anything else under a
/// format registered by its MIME type. Busy when another process held the
/// clipboard open.
Result setData(const std::vector<ClipboardData>& items);

/// CF_UNICODETEXT (else CF_TEXT) as UTF-8 with LF line ends; "" when the
/// clipboard holds no text. `*busy` when the clipboard could not be opened.
std::string getText(bool* busy);

/// "image/png" (the "PNG" format), "image/bmp" (CF_DIBV5 / CF_DIB as a BMP
/// file) or a format registered under the MIME type. nullopt when absent;
/// `*busy` when the clipboard could not be opened.
std::optional<std::vector<uint8_t>> getData(const std::string& mimeType, bool* busy);

}  // namespace bro::platform::win32clip
