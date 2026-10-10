#pragma once
// The system clipboard (and, on X11/Wayland, the primary selection).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace bro::platform {

/// One representation of a clipboard item: bytes under a MIME type.
/// "text/plain" is UTF-8 text; "image/png" a PNG file.
struct ClipboardData {
    std::string mimeType;
    std::vector<uint8_t> bytes;
};

class Clipboard {
public:
    virtual ~Clipboard() = default;

    /// Replace the clipboard with `text`. False when the clipboard could not
    /// be written — callers must honour that: a `copy` that reports success
    /// it did not have is a paste of somebody else's text later; a `cut` that
    /// does is the text deleted and nowhere to paste it back from.
    virtual bool setText(const std::string& text) = 0;

    /// Replace the clipboard with several representations of one item at
    /// once (text and an image, say), each offered under its MIME type the
    /// way the OS spells it: "text/plain" as the OS's text, "image/png" as
    /// PNG plus whatever bitmap format the OS's own apps paste (CF_DIB /
    /// CF_DIBV5 on Windows). Other types are offered as raw bytes under
    /// their own name. False when the clipboard could not be written.
    virtual bool setData(const std::vector<ClipboardData>& items) = 0;

    /// The clipboard's text. An empty clipboard is a success with "", and
    /// only a read that never got in is a failure (`*ok = false`).
    virtual std::string getText(bool* ok = nullptr) = 0;

    /// Non-text contents offered under a MIME type (an image a screenshot
    /// tool copied, say). nullopt when the clipboard holds nothing of it.
    /// "image/bmp" is a whole BMP file (on Windows: CF_DIBV5 / CF_DIB with
    /// a file header put in front).
    virtual std::optional<std::vector<uint8_t>> getData(const std::string& mimeType) = 0;

    /// The primary selection (select-to-copy, middle-click paste). Where the
    /// OS has none, setPrimaryText is false and getPrimaryText nullopt, and
    /// the caller keeps its own.
    virtual bool setPrimaryText(const std::string& text) = 0;
    virtual std::optional<std::string> getPrimaryText() = 0;
};

/// The active clipboard: the WindowSystem's, except in headless, where it is
/// an in-process one (tests never touch the machine's clipboard) unless
/// $BRO_HEADLESS_SYSTEM_CLIPBOARD=1 asks for the real one.
Clipboard& clipboard();

/// A clipboard that lives in this process: the DRM shell's, and headless's.
Clipboard& localClipboard();

// The clipboard's text, the common case.
bool setClipboardText(const std::string& text);
std::string getClipboardText(bool* ok = nullptr);

/// The clipboard's image as a PNG file, whatever format it is held in: PNG
/// as it is; a bitmap (CF_DIB, image/bmp), JPEG or GIF decoded and
/// re-encoded. nullopt when the clipboard holds no image bro can decode.
std::optional<std::vector<uint8_t>> getClipboardImagePng();

// --- conversions the OS backends share (clipboard_image.cpp) ---

/// A packed DIB (BITMAPINFOHEADER or a V4/V5 header, its masks / colour
/// table, then pixels: what CF_DIB / CF_DIBV5 hold) as a BMP file.
std::optional<std::vector<uint8_t>> dibToBmpFile(const uint8_t* dib, size_t size);

/// RGBA8 pixels as a packed 32-bit bottom-up DIB: a BITMAPV5HEADER with
/// BI_BITFIELDS and an alpha mask when `v5`, else a BITMAPINFOHEADER with
/// BI_RGB (the alpha byte kept, which CF_DIB readers treat as reserved).
std::vector<uint8_t> rgbaToDib(const uint8_t* rgba, int width, int height, bool v5);

/// Decode an image file (PNG, BMP, JPEG, GIF, ...) to RGBA8.
bool decodeClipboardImage(const std::vector<uint8_t>& file, std::vector<uint8_t>& rgba,
                          int& width, int& height);

} // namespace bro::platform
