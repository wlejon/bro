// The Windows clipboard's non-text half, in Win32 directly.
//
// SDL3's Windows backend writes ONE image format (the first image MIME type
// offered: PNG, or a BMP as CF_DIB) and needs the video subsystem for any of
// it. An image another app can paste wants both at once: the registered "PNG"
// format (browsers, Office, Slack read it, keeping alpha) and CF_DIBV5 /
// CF_DIB (Paint and most native apps read only those). So a write here puts
// every representation up in one OpenClipboard: CF_UNICODETEXT for text,
// "PNG" + CF_DIBV5 + CF_DIB for a PNG, and a registered format named by the
// MIME type for anything else. Reading answers "image/png" from "PNG" and
// "image/bmp" from whichever of CF_DIBV5 / CF_DIB the owner put up first (the
// other is the system's synthesis of it), as a BMP file.
//
// The memory blocks are all made before the clipboard is opened, so it is
// held for as short a time as the copy itself takes. A clipboard somebody
// else holds is reported as Busy and the caller retries (sdl_clipboard.cpp).
#ifdef _WIN32

#include "platform/sdl/win32_clipboard.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>

namespace bro::platform::win32clip {

namespace {

UINT pngFormat() {
    static const UINT f = RegisterClipboardFormatW(L"PNG");
    return f;
}

HGLOBAL toGlobal(const void* data, size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size ? size : 1);
    if (!h) return nullptr;
    void* p = GlobalLock(h);
    if (!p) {
        GlobalFree(h);
        return nullptr;
    }
    if (size) std::memcpy(p, data, size);
    GlobalUnlock(h);
    return h;
}

// UTF-8 to NUL-terminated UTF-16 with CRLF line ends: what CF_UNICODETEXT
// readers (Notepad, every edit control) expect.
HGLOBAL textGlobal(const std::vector<uint8_t>& utf8) {
    std::wstring w;
    if (!utf8.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(utf8.data()),
                                          int(utf8.size()), nullptr, 0);
        w.resize(size_t(n));
        MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(utf8.data()), int(utf8.size()),
                            w.data(), n);
    }
    std::wstring crlf;
    crlf.reserve(w.size() + 8);
    for (size_t i = 0; i < w.size(); ++i) {
        if (w[i] == L'\n' && (i == 0 || w[i - 1] != L'\r')) crlf.push_back(L'\r');
        crlf.push_back(w[i]);
    }
    return toGlobal(crlf.c_str(), (crlf.size() + 1) * sizeof(wchar_t));
}

// A PNG's length up to the end of its IEND chunk: GlobalSize rounds a block
// up, and the bytes past IEND are not the image's.
size_t pngLength(const uint8_t* p, size_t size) {
    size_t off = 8;
    while (off + 12 <= size) {
        const uint32_t len = (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) |
                             (uint32_t(p[off + 2]) << 8) | uint32_t(p[off + 3]);
        const size_t end = off + 12 + size_t(len);
        if (end > size) break;
        if (std::memcmp(p + off + 4, "IEND", 4) == 0) return end;
        off = end;
    }
    return size;
}

bool openClipboard() { return OpenClipboard(nullptr) != 0; }

std::optional<std::vector<uint8_t>> globalBytes(UINT format) {
    HANDLE h = GetClipboardData(format);
    if (!h) return std::nullopt;
    const size_t size = GlobalSize(h);
    const void* p = GlobalLock(h);
    if (!p) return std::nullopt;
    auto* b = static_cast<const uint8_t*>(p);
    std::vector<uint8_t> out(b, b + size);
    GlobalUnlock(h);
    return out;
}

}  // namespace

Result setData(const std::vector<ClipboardData>& items) {
    struct Entry { UINT format; HGLOBAL mem; };
    std::vector<Entry> entries;
    bool built = true;
    auto add = [&](UINT format, HGLOBAL mem) {
        if (format && mem) entries.push_back({format, mem});
        else {
            if (mem) GlobalFree(mem);
            built = false;
        }
    };
    for (const auto& it : items) {
        if (it.mimeType == "text/plain") {
            add(CF_UNICODETEXT, textGlobal(it.bytes));
        } else if (it.mimeType == "image/png") {
            add(pngFormat(), toGlobal(it.bytes.data(), it.bytes.size()));
            std::vector<uint8_t> rgba;
            int w = 0, h = 0;
            if (decodeClipboardImage(it.bytes, rgba, w, h)) {
                const auto v5 = rgbaToDib(rgba.data(), w, h, true);
                const auto v3 = rgbaToDib(rgba.data(), w, h, false);
                add(CF_DIB, toGlobal(v3.data(), v3.size()));
                add(CF_DIBV5, toGlobal(v5.data(), v5.size()));
            }
        } else {
            add(RegisterClipboardFormatA(it.mimeType.c_str()), toGlobal(it.bytes.data(), it.bytes.size()));
        }
    }
    auto freeAll = [&] {
        for (auto& e : entries) if (e.mem) GlobalFree(e.mem);
    };
    if (!built) {
        freeAll();
        return Result::Failed;
    }
    if (!openClipboard()) {
        freeAll();
        return Result::Busy;
    }
    EmptyClipboard();
    bool ok = true;
    for (auto& e : entries) {
        // On success the system owns the block; on failure it stays ours.
        if (SetClipboardData(e.format, e.mem)) e.mem = nullptr;
        else ok = false;
    }
    CloseClipboard();
    freeAll();
    return ok ? Result::Ok : Result::Failed;
}

std::string getText(bool* busy) {
    if (busy) *busy = false;
    const bool unicode = IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
    if (!unicode && !IsClipboardFormatAvailable(CF_TEXT)) return std::string();
    if (!openClipboard()) {
        if (busy) *busy = true;
        return std::string();
    }
    std::string out;
    if (HANDLE h = GetClipboardData(unicode ? CF_UNICODETEXT : CF_TEXT)) {
        if (const void* p = GlobalLock(h)) {
            const size_t cap = GlobalSize(h);
            if (unicode) {
                const auto* w = static_cast<const wchar_t*>(p);
                size_t n = 0;
                while (n < cap / sizeof(wchar_t) && w[n]) ++n;
                if (n) {
                    const int len = WideCharToMultiByte(CP_UTF8, 0, w, int(n), nullptr, 0, nullptr, nullptr);
                    out.resize(size_t(len));
                    WideCharToMultiByte(CP_UTF8, 0, w, int(n), out.data(), len, nullptr, nullptr);
                }
            } else {
                const auto* c = static_cast<const char*>(p);
                size_t n = 0;
                while (n < cap && c[n]) ++n;
                out.assign(c, n);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    // Text went up with CRLF line ends (textGlobal); it comes back as the
    // page wrote it.
    std::string lf;
    lf.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r' && i + 1 < out.size() && out[i + 1] == '\n') continue;
        lf.push_back(out[i]);
    }
    return lf;
}

std::optional<std::vector<uint8_t>> getData(const std::string& mimeType, bool* busy) {
    if (busy) *busy = false;
    UINT format = 0;
    if (mimeType == "image/png") {
        format = pngFormat();
        if (!IsClipboardFormatAvailable(format)) {
            // Some writers register the MIME type itself.
            format = RegisterClipboardFormatA("image/png");
            if (!IsClipboardFormatAvailable(format)) return std::nullopt;
        }
    } else if (mimeType == "image/bmp") {
        if (!IsClipboardFormatAvailable(CF_DIBV5) && !IsClipboardFormatAvailable(CF_DIB))
            return std::nullopt;
    } else {
        format = RegisterClipboardFormatA(mimeType.c_str());
        if (!format || !IsClipboardFormatAvailable(format)) return std::nullopt;
    }
    if (!openClipboard()) {
        if (busy) *busy = true;
        return std::nullopt;
    }
    std::optional<std::vector<uint8_t>> out;
    if (mimeType == "image/bmp") {
        // The format the owner put up comes first in the enumeration; the
        // other one is the system's synthesis of it.
        UINT dib = 0;
        for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) {
            if (f == CF_DIB || f == CF_DIBV5) {
                dib = f;
                break;
            }
        }
        if (dib) {
            if (auto raw = globalBytes(dib)) out = dibToBmpFile(raw->data(), raw->size());
        }
    } else {
        out = globalBytes(format);
        if (out && mimeType == "image/png") out->resize(pngLength(out->data(), out->size()));
    }
    CloseClipboard();
    return out;
}

}  // namespace bro::platform::win32clip

#endif  // _WIN32
