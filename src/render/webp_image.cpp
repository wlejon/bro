#include "render/webp_image.h"

#include <webp/decode.h>

#include <cstdio>

namespace bro::render {

bool decodeWebP(const void* data, std::size_t len,
                int& width, int& height, std::vector<uint8_t>& out) {
    if (!data || len == 0) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);

    // WebPGetInfo doubles as the format check: it validates the RIFF/WEBP
    // container and the VP8/VP8L/VP8X chunk header, so non-WebP bytes and
    // truncated files are both rejected here rather than part-decoded. That
    // is why there is no separate signature sniff.
    int w = 0, h = 0;
    if (!WebPGetInfo(bytes, len, &w, &h)) return false;
    if (w <= 0 || h <= 0) return false;

    uint8_t* pixels = WebPDecodeRGBA(bytes, len, &w, &h);
    if (!pixels) return false;

    // Copy into the caller's vector and release libwebp's buffer immediately.
    // Handing the raw pointer out instead would make every caller responsible
    // for calling WebPFree (not free()) on exactly the right paths, which is
    // a trap for the price of one memcpy on a decode that already cost far
    // more than that.
    const std::size_t bytesOut = static_cast<std::size_t>(w) * h * 4;
    out.assign(pixels, pixels + bytesOut);
    WebPFree(pixels);

    width = w;
    height = h;
    return true;
}

bool decodeWebPFile(const std::string& path,
                    int& width, int& height, std::vector<uint8_t>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    std::vector<uint8_t> bytes;
    // A WebP that isn't one is the common case here (this is a fallback, so
    // most calls arrive with PNG/JPEG bytes); read the whole file anyway
    // rather than sniffing first, since the header check inside decodeWebP is
    // the authoritative one and these files are small.
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    if (size <= 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);
    bytes.resize(static_cast<std::size_t>(size));
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) return false;

    return decodeWebP(bytes.data(), bytes.size(), width, height, out);
}

} // namespace bro::render
