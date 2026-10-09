#pragma once
// The system clipboard (and, on X11/Wayland, the primary selection).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bro::platform {

class Clipboard {
public:
    virtual ~Clipboard() = default;

    /// Replace the clipboard with `text`. False when the clipboard could not
    /// be written — callers must honour that: a `copy` that reports success
    /// it did not have is a paste of somebody else's text later; a `cut` that
    /// does is the text deleted and nowhere to paste it back from.
    virtual bool setText(const std::string& text) = 0;

    /// The clipboard's text. An empty clipboard is a success with "", and
    /// only a read that never got in is a failure (`*ok = false`).
    virtual std::string getText(bool* ok = nullptr) = 0;

    /// Non-text contents offered under a MIME type (an image a screenshot
    /// tool copied, say). nullopt when the clipboard holds nothing of it.
    virtual std::optional<std::vector<uint8_t>> getData(const std::string& mimeType) = 0;

    /// The primary selection (select-to-copy, middle-click paste). Where the
    /// OS has none, setPrimaryText is false and getPrimaryText nullopt, and
    /// the caller keeps its own.
    virtual bool setPrimaryText(const std::string& text) = 0;
    virtual std::optional<std::string> getPrimaryText() = 0;
};

/// The active WindowSystem's clipboard.
Clipboard& clipboard();

// The clipboard's text, the common case.
bool setClipboardText(const std::string& text);
std::string getClipboardText(bool* ok = nullptr);

} // namespace bro::platform
