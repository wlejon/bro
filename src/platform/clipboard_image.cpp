// What every clipboard backend shares: the in-process clipboard (the DRM
// shell's and headless's), the image read that turns whatever bitmap format
// the OS holds into PNG, and the DIB <-> BMP / RGBA conversions the Windows
// backend needs (kept here, in plain byte arithmetic, so they compile and are
// reachable everywhere).
#include "platform/clipboard.h"

#include <broimage/decode.h>
#include <broimage/encode.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace bro::platform {

namespace {

uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
void wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
void wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

constexpr uint32_t kBiRgb = 0, kBiBitfields = 3, kBiAlphaBitfields = 6;

bool isPng(const std::vector<uint8_t>& b) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return b.size() > 8 && std::memcmp(b.data(), sig, 8) == 0;
}

// One clipboard item held in memory, every representation of it.
class LocalClipboard final : public Clipboard {
public:
    bool setText(const std::string& text) override {
        std::lock_guard<std::mutex> g(m_mu);
        m_items.clear();
        m_items.push_back({"text/plain", std::vector<uint8_t>(text.begin(), text.end())});
        return true;
    }
    bool setData(const std::vector<ClipboardData>& items) override {
        std::lock_guard<std::mutex> g(m_mu);
        m_items = items;
        return true;
    }
    std::string getText(bool* ok) override {
        std::lock_guard<std::mutex> g(m_mu);
        if (ok) *ok = true;
        for (const auto& it : m_items)
            if (it.mimeType == "text/plain") return std::string(it.bytes.begin(), it.bytes.end());
        return std::string();
    }
    std::optional<std::vector<uint8_t>> getData(const std::string& mimeType) override {
        std::lock_guard<std::mutex> g(m_mu);
        for (const auto& it : m_items)
            if (it.mimeType == mimeType) return it.bytes;
        return std::nullopt;
    }
    bool setPrimaryText(const std::string&) override { return false; }
    std::optional<std::string> getPrimaryText() override { return std::nullopt; }

private:
    std::mutex m_mu;
    std::vector<ClipboardData> m_items;
};

}  // namespace

Clipboard& localClipboard() {
    static LocalClipboard c;
    return c;
}

bool decodeClipboardImage(const std::vector<uint8_t>& file, std::vector<uint8_t>& rgba,
                          int& width, int& height) {
    if (file.empty()) return false;
    broimage::Image img;
    if (!broimage::decode_memory(file.data(), file.size(), img)) return false;
    if (img.width <= 0 || img.height <= 0 || img.channels != 4) return false;
    width = img.width;
    height = img.height;
    rgba = std::move(img.pixels);
    return true;
}

std::optional<std::vector<uint8_t>> getClipboardImagePng() {
    Clipboard& c = clipboard();
    if (auto png = c.getData("image/png"); png && isPng(*png)) return png;
    // A bitmap (a Windows screenshot is CF_DIB, which reads as image/bmp),
    // or what a browser or image viewer copied, re-encoded as PNG.
    for (const char* mime : {"image/bmp", "image/jpeg", "image/gif", "image/webp", "image/tiff"}) {
        auto bytes = c.getData(mime);
        if (!bytes || bytes->empty()) continue;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        if (!decodeClipboardImage(*bytes, rgba, w, h)) continue;
        std::vector<uint8_t> png;
        if (broimage::encode_png_memory(png, rgba.data(), w, h, 4)) return png;
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> dibToBmpFile(const uint8_t* dib, size_t size) {
    if (!dib || size < 40) return std::nullopt;
    const uint32_t hdr = rd32(dib);
    if (hdr < 40 || hdr > size) return std::nullopt;
    const int32_t width = int32_t(rd32(dib + 4));
    const int32_t height = int32_t(rd32(dib + 8));
    const uint16_t bpp = rd16(dib + 14);
    const uint32_t compression = rd32(dib + 16);
    const uint32_t clrUsed = rd32(dib + 32);
    if (width <= 0 || height == 0) return std::nullopt;

    // What sits between the header and the pixels: the masks a 40-byte
    // header carries after itself (a V4/V5 header holds them inside), and
    // the colour table.
    size_t table = 0;
    if (compression == kBiBitfields && hdr == 40) table += 12;
    else if (compression == kBiAlphaBitfields && hdr == 40) table += 16;
    if (bpp <= 8) table += 4u * (clrUsed ? clrUsed : (1u << bpp));
    else table += 4u * size_t(clrUsed);
    const size_t offBits = 14 + hdr + table;
    if (hdr + table > size) return std::nullopt;

    std::vector<uint8_t> out(14 + size);
    out[0] = 'B';
    out[1] = 'M';
    wr32(out.data() + 2, uint32_t(out.size()));
    wr32(out.data() + 6, 0);
    wr32(out.data() + 10, uint32_t(offBits));
    std::memcpy(out.data() + 14, dib, size);
    return out;
}

std::vector<uint8_t> rgbaToDib(const uint8_t* rgba, int width, int height, bool v5) {
    const uint32_t hdr = v5 ? 124 : 40;
    const size_t pixelBytes = size_t(width) * size_t(height) * 4;
    std::vector<uint8_t> out(hdr + pixelBytes, 0);
    uint8_t* h = out.data();
    wr32(h + 0, hdr);
    wr32(h + 4, uint32_t(width));
    wr32(h + 8, uint32_t(height));  // positive: bottom-up, what every reader takes
    wr16(h + 12, 1);
    wr16(h + 14, 32);
    wr32(h + 16, v5 ? kBiBitfields : kBiRgb);
    wr32(h + 20, uint32_t(pixelBytes));
    wr32(h + 24, 2835);  // 72 dpi
    wr32(h + 28, 2835);
    if (v5) {
        wr32(h + 40, 0x00FF0000);  // red
        wr32(h + 44, 0x0000FF00);  // green
        wr32(h + 48, 0x000000FF);  // blue
        wr32(h + 52, 0xFF000000);  // alpha
        wr32(h + 56, 0x73524742);  // LCS_sRGB
        wr32(h + 108, 4);          // LCS_GM_IMAGES
    }
    uint8_t* px = out.data() + hdr;
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = rgba + size_t(height - 1 - y) * size_t(width) * 4;
        uint8_t* dst = px + size_t(y) * size_t(width) * 4;
        for (int x = 0; x < width; ++x, src += 4, dst += 4) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = src[3];
        }
    }
    return out;
}

}  // namespace bro::platform
