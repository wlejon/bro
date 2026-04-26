#include "video/gif_encoder.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace bro::video {

namespace {

// ---------- Median-cut palette quantization --------------------------------
//
// Reduces an RGBA frame (alpha snapped to 0/255) to ≤256 indexed colors with
// a per-frame palette. Index 0 is reserved for the transparent color so the
// GIF graphic-control extension can mark it. Opaque pixels are mapped to
// the nearest palette entry by squared-RGB distance.
//
// Median cut: stuff every opaque pixel into one big box, repeatedly split
// the box with the widest channel range at its median, until we hit the
// target number of boxes; each box's average becomes a palette entry. Good
// enough for sprite art; not perceptually weighted, no dithering.

struct RGBPixel { uint8_t r, g, b; };

struct ColorBox {
    std::vector<int> idx;   // indices into the source pixel array
    uint8_t rMin, rMax, gMin, gMax, bMin, bMax;
};

void boxBounds(const std::vector<RGBPixel>& src, ColorBox& box) {
    box.rMin = box.gMin = box.bMin = 255;
    box.rMax = box.gMax = box.bMax = 0;
    for (int i : box.idx) {
        const auto& p = src[i];
        if (p.r < box.rMin) box.rMin = p.r;
        if (p.r > box.rMax) box.rMax = p.r;
        if (p.g < box.gMin) box.gMin = p.g;
        if (p.g > box.gMax) box.gMax = p.g;
        if (p.b < box.bMin) box.bMin = p.b;
        if (p.b > box.bMax) box.bMax = p.b;
    }
}

int boxLongestSide(const ColorBox& b) {
    const int r = b.rMax - b.rMin;
    const int g = b.gMax - b.gMin;
    const int bl = b.bMax - b.bMin;
    return std::max({r, g, bl});
}

void medianCut(const std::vector<RGBPixel>& opaque,
               int targetColors,
               std::vector<RGBPixel>& palette) {
    palette.clear();
    if (opaque.empty()) return;
    if (targetColors < 1) targetColors = 1;
    if (targetColors > 256) targetColors = 256;

    std::vector<ColorBox> boxes;
    boxes.reserve(targetColors);
    {
        ColorBox b;
        b.idx.resize(opaque.size());
        for (size_t i = 0; i < opaque.size(); ++i) b.idx[i] = static_cast<int>(i);
        boxBounds(opaque, b);
        boxes.push_back(std::move(b));
    }

    // Repeatedly split the box with the widest channel range until we have
    // targetColors boxes (or every box has zero range / one pixel).
    while (static_cast<int>(boxes.size()) < targetColors) {
        int best = -1, bestSide = 0;
        for (size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].idx.size() < 2) continue;
            const int side = boxLongestSide(boxes[i]);
            if (side > bestSide) { bestSide = side; best = static_cast<int>(i); }
        }
        if (best < 0 || bestSide == 0) break;

        ColorBox& src = boxes[best];
        const int rRange = src.rMax - src.rMin;
        const int gRange = src.gMax - src.gMin;
        const int bRange = src.bMax - src.bMin;
        const char axis = (rRange >= gRange && rRange >= bRange) ? 'r'
                        : (gRange >= bRange) ? 'g' : 'b';

        std::sort(src.idx.begin(), src.idx.end(), [&](int a, int b) {
            const auto& pa = opaque[a]; const auto& pb = opaque[b];
            switch (axis) {
                case 'r': return pa.r < pb.r;
                case 'g': return pa.g < pb.g;
                default:  return pa.b < pb.b;
            }
        });

        const size_t mid = src.idx.size() / 2;
        ColorBox lo, hi;
        lo.idx.assign(src.idx.begin(), src.idx.begin() + mid);
        hi.idx.assign(src.idx.begin() + mid, src.idx.end());
        boxBounds(opaque, lo);
        boxBounds(opaque, hi);
        boxes[best] = std::move(lo);
        boxes.push_back(std::move(hi));
    }

    palette.reserve(boxes.size());
    for (const auto& b : boxes) {
        if (b.idx.empty()) continue;
        uint64_t r = 0, g = 0, bs = 0;
        for (int i : b.idx) {
            const auto& p = opaque[i];
            r += p.r; g += p.g; bs += p.b;
        }
        const size_t n = b.idx.size();
        palette.push_back({
            static_cast<uint8_t>(r / n),
            static_cast<uint8_t>(g / n),
            static_cast<uint8_t>(bs / n),
        });
    }
}

// Map every pixel in the source frame to a palette index by nearest-neighbor
// (squared RGB distance). Opaque pixels search palette[paletteOffset..]; the
// transparent pixels become index 0. The first opaque palette entry sits at
// `paletteOffset` so index 0 stays reserved for transparent.
void mapToIndices(const uint8_t* rgba, int srcStride, int w, int h,
                  const std::vector<RGBPixel>& palette, int paletteOffset,
                  std::vector<uint8_t>& outIndices) {
    outIndices.assign(static_cast<size_t>(w) * h, 0);
    if (palette.empty()) return;

    for (int y = 0; y < h; ++y) {
        const uint8_t* src = rgba + y * srcStride;
        uint8_t* dst = outIndices.data() + y * w;
        for (int x = 0; x < w; ++x) {
            const uint8_t a = src[3];
            if (a < 128) { dst[x] = 0; src += 4; continue; }
            const int r = src[0], g = src[1], b = src[2];
            int bestIdx = paletteOffset;
            int bestDist = INT32_MAX;
            for (size_t i = 0; i < palette.size(); ++i) {
                const auto& p = palette[i];
                const int dr = r - p.r, dg = g - p.g, db = b - p.b;
                const int d = dr*dr + dg*dg + db*db;
                if (d < bestDist) {
                    bestDist = d;
                    bestIdx = static_cast<int>(i) + paletteOffset;
                    if (d == 0) break;
                }
            }
            dst[x] = static_cast<uint8_t>(bestIdx);
            src += 4;
        }
    }
}

// ---------- LZW encoder for GIF image data ---------------------------------
//
// GIF LZW uses variable-width codes starting at min_code_size+1 bits, with
// reserved CLEAR (1<<min_code_size) and END (CLEAR+1) codes. Output is split
// into sub-blocks of up to 255 bytes each, prefixed by a length byte.

class BitWriter {
public:
    void writeBits(uint32_t code, int nBits) {
        bitBuf_ |= static_cast<uint64_t>(code) << bitCount_;
        bitCount_ += nBits;
        while (bitCount_ >= 8) {
            bytes_.push_back(static_cast<uint8_t>(bitBuf_ & 0xFF));
            bitBuf_ >>= 8;
            bitCount_ -= 8;
        }
    }
    void flushBitsToByte() {
        if (bitCount_ > 0) {
            bytes_.push_back(static_cast<uint8_t>(bitBuf_ & 0xFF));
            bitBuf_ = 0;
            bitCount_ = 0;
        }
    }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
private:
    std::vector<uint8_t> bytes_;
    uint64_t bitBuf_ = 0;
    int bitCount_ = 0;
};

void writeSubBlocks(FILE* f, const std::vector<uint8_t>& data) {
    size_t pos = 0;
    while (pos < data.size()) {
        const size_t n = std::min<size_t>(255, data.size() - pos);
        const uint8_t len = static_cast<uint8_t>(n);
        std::fwrite(&len, 1, 1, f);
        std::fwrite(data.data() + pos, 1, n, f);
        pos += n;
    }
    const uint8_t zero = 0;
    std::fwrite(&zero, 1, 1, f);
}

void encodeLZW(const std::vector<uint8_t>& indices, int minCodeSize, FILE* f) {
    // Write the LZW min code size as a one-byte header before the sub-blocks.
    const uint8_t minCs = static_cast<uint8_t>(minCodeSize);
    std::fwrite(&minCs, 1, 1, f);

    BitWriter bw;
    const int clearCode = 1 << minCodeSize;
    const int endCode   = clearCode + 1;
    int codeSize = minCodeSize + 1;
    int nextCode = endCode + 1;
    const int maxCode = 1 << 12;

    // Dictionary keyed on (prefix << 8) | suffix. 4096-entry table is the GIF
    // spec maximum; an unordered_map is fine for our frame sizes — sprites
    // hit the table size cap rarely.
    auto makeKey = [](int prefix, int suffix) -> uint32_t {
        return (static_cast<uint32_t>(prefix) << 8) | static_cast<uint8_t>(suffix);
    };
    std::vector<int> table(1 << 20, -1);   // sparse direct-addressing table
    auto resetTable = [&](){ std::fill(table.begin(), table.end(), -1); };

    bw.writeBits(clearCode, codeSize);
    if (indices.empty()) {
        bw.writeBits(endCode, codeSize);
        bw.flushBitsToByte();
        writeSubBlocks(f, bw.bytes());
        return;
    }

    int prefix = indices[0];
    for (size_t i = 1; i < indices.size(); ++i) {
        const int suffix = indices[i];
        const uint32_t key = makeKey(prefix, suffix);
        const int existing = table[key];
        if (existing >= 0) {
            prefix = existing;
        } else {
            bw.writeBits(prefix, codeSize);
            if (nextCode < maxCode) {
                table[key] = nextCode;
                // Bump width when the value we just assigned would need a
                // wider code if it were ever used as a future prefix. This
                // matches the gif.h / giflib convention; strict decoders
                // (Pillow) require this exact ordering.
                if (codeSize < 12 && nextCode == (1 << codeSize)) {
                    ++codeSize;
                }
                ++nextCode;
            } else {
                bw.writeBits(clearCode, codeSize);
                resetTable();
                codeSize = minCodeSize + 1;
                nextCode = endCode + 1;
            }
            prefix = suffix;
        }
    }
    bw.writeBits(prefix, codeSize);
    bw.writeBits(endCode, codeSize);
    bw.flushBitsToByte();
    writeSubBlocks(f, bw.bytes());
}

// ---------- GIF89a writer ---------------------------------------------------

void writeU16LE(FILE* f, uint16_t v) {
    uint8_t b[2] = { static_cast<uint8_t>(v & 0xFF),
                     static_cast<uint8_t>((v >> 8) & 0xFF) };
    std::fwrite(b, 1, 2, f);
}

class GifEncoderImpl final : public GifEncoder {
public:
    GifEncoderImpl() = default;
    ~GifEncoderImpl() override { finish(); }

    bool open(const std::string& path, const Config& cfg, std::string* err) {
        cfg_ = cfg;
        if (cfg_.width <= 0 || cfg_.height <= 0) {
            setErr("width/height must be positive"); if (err) *err = lastErr_;
            return false;
        }
        if (cfg_.paletteBits < 1) cfg_.paletteBits = 1;
        if (cfg_.paletteBits > 8) cfg_.paletteBits = 8;
        if (cfg_.delayCs < 0) cfg_.delayCs = 0;
        nextDelayCs_ = cfg_.delayCs;

        f_ = std::fopen(path.c_str(), "wb");
        if (!f_) { setErr("fopen failed: " + path); if (err) *err = lastErr_; return false; }

        // Header
        std::fwrite("GIF89a", 1, 6, f_);
        // Logical Screen Descriptor
        writeU16LE(f_, static_cast<uint16_t>(cfg_.width));
        writeU16LE(f_, static_cast<uint16_t>(cfg_.height));
        // packed: no global color table, color resolution = 7
        const uint8_t packed = 0x70;
        std::fwrite(&packed, 1, 1, f_);
        const uint8_t bg = 0, ratio = 0;
        std::fwrite(&bg, 1, 1, f_);
        std::fwrite(&ratio, 1, 1, f_);

        // NETSCAPE 2.0 application extension (loop forever / N times)
        writeNetscapeLoop(cfg_.loopCount);
        return true;
    }

    bool addFrameRGBA(const uint8_t* rgba, int srcStride) override {
        if (!f_) { setErr("encoder closed"); return false; }
        if (srcStride <= 0) srcStride = cfg_.width * 4;

        const int targetColors = (1 << cfg_.paletteBits) - 1;  // -1 for transparent slot
        std::vector<RGBPixel> opaque;
        opaque.reserve(static_cast<size_t>(cfg_.width) * cfg_.height);
        for (int y = 0; y < cfg_.height; ++y) {
            const uint8_t* src = rgba + y * srcStride;
            for (int x = 0; x < cfg_.width; ++x) {
                if (src[3] >= 128) {
                    opaque.push_back({src[0], src[1], src[2]});
                }
                src += 4;
            }
        }

        std::vector<RGBPixel> palette;
        medianCut(opaque, targetColors, palette);

        // Build the local color table: entry 0 = transparent placeholder
        // (any color, here magenta — never written to a visible pixel),
        // entries 1..N = quantized colors.
        // GIF requires palette size to be a power of 2.
        std::vector<RGBPixel> ct;
        ct.reserve(256);
        ct.push_back({0xFF, 0x00, 0xFF});
        for (const auto& p : palette) ct.push_back(p);
        // Round up to nearest power of 2 within paletteBits cap.
        int ctBits = 1;
        while ((1 << ctBits) < static_cast<int>(ct.size())) ++ctBits;
        if (ctBits < 2) ctBits = 2;     // GIF min is 2 bits → 4 entries
        if (ctBits > cfg_.paletteBits) ctBits = cfg_.paletteBits;
        const int ctSize = 1 << ctBits;
        ct.resize(ctSize, {0, 0, 0});

        std::vector<uint8_t> indices;
        mapToIndices(rgba, srcStride, cfg_.width, cfg_.height, palette, 1, indices);

        writeFrameHeader(ctBits);
        // Local Color Table
        for (const auto& p : ct) {
            const uint8_t row[3] = {p.r, p.g, p.b};
            std::fwrite(row, 1, 3, f_);
        }
        // GIF spec: min code size must be >= 2.
        const int minCs = std::max(2, ctBits);
        encodeLZW(indices, minCs, f_);
        ++framesWritten_;
        nextDelayCs_ = cfg_.delayCs;
        return true;
    }

    void setNextFrameDelayCs(int delayCs) override {
        if (delayCs < 0) delayCs = 0;
        nextDelayCs_ = delayCs;
    }

    bool finish() override {
        if (!f_) return true;
        const uint8_t trailer = 0x3B;
        std::fwrite(&trailer, 1, 1, f_);
        std::fclose(f_);
        f_ = nullptr;
        return true;
    }

    const std::string& lastError() const override { return lastErr_; }
    int framesWritten() const override { return framesWritten_; }

private:
    void setErr(const std::string& s) { lastErr_ = s; }

    void writeNetscapeLoop(int loopCount) {
        // Application Extension: NETSCAPE2.0, loop sub-block.
        const uint8_t ext[] = {
            0x21, 0xFF, 0x0B,
            'N','E','T','S','C','A','P','E','2','.','0',
            0x03, 0x01,
            static_cast<uint8_t>(loopCount & 0xFF),
            static_cast<uint8_t>((loopCount >> 8) & 0xFF),
            0x00,
        };
        std::fwrite(ext, 1, sizeof(ext), f_);
    }

    void writeFrameHeader(int ctBits) {
        // Graphic Control Extension: transparency on, transparent index 0.
        const uint8_t gce[] = {
            0x21, 0xF9, 0x04,
            0x05,                                                // packed: disposal=0, transparency flag set
            static_cast<uint8_t>(nextDelayCs_ & 0xFF),
            static_cast<uint8_t>((nextDelayCs_ >> 8) & 0xFF),
            0x00,                                                // transparent color index
            0x00,
        };
        std::fwrite(gce, 1, sizeof(gce), f_);

        // Image Descriptor.
        const uint8_t imageSep = 0x2C;
        std::fwrite(&imageSep, 1, 1, f_);
        writeU16LE(f_, 0);                                       // left
        writeU16LE(f_, 0);                                       // top
        writeU16LE(f_, static_cast<uint16_t>(cfg_.width));
        writeU16LE(f_, static_cast<uint16_t>(cfg_.height));
        // packed: local color table flag + ctBits-1 in low 3 bits
        const uint8_t packed = static_cast<uint8_t>(0x80 | ((ctBits - 1) & 0x07));
        std::fwrite(&packed, 1, 1, f_);
    }

    Config cfg_{};
    FILE* f_ = nullptr;
    int framesWritten_ = 0;
    int nextDelayCs_ = 4;
    std::string lastErr_;
};

} // namespace

std::unique_ptr<GifEncoder> GifEncoder::create(const std::string& path,
                                               const Config& cfg,
                                               std::string* err) {
    auto enc = std::make_unique<GifEncoderImpl>();
    if (!enc->open(path, cfg, err)) return nullptr;
    return enc;
}

} // namespace bro::video
