#include "webgl/vulkan/webgl_vk_pixels.h"
#include "webgl/vulkan/webgl_vk_formats.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bro::webgl::vk {

namespace {

template <typename T>
T load(const uint8_t* p, size_t index = 0) {
    T v;
    std::memcpy(&v, p + index * sizeof(T), sizeof(T));
    return v;
}

template <typename T>
void store(uint8_t* p, size_t index, T v) {
    std::memcpy(p + index * sizeof(T), &v, sizeof(T));
}

// An unsigned float of a 5-bit exponent and `mantBits` mantissa bits.
uint32_t toSmallFloat(float f, uint32_t mantBits) {
    if (std::isnan(f)) return (31u << mantBits) | 1u;
    if (f <= 0.0f) return 0;
    const uint32_t maxBits = (30u << mantBits) | ((1u << mantBits) - 1);
    if (std::isinf(f)) return 31u << mantBits;
    int exp = 0;
    const float m = std::frexp(f, &exp);  // f = m * 2^exp, m in [0.5, 1)
    int e = exp - 1 + 15;
    if (e <= 0) {  // denormal: f = mant * 2^(-14 - mantBits)
        const auto mant = static_cast<uint32_t>(std::lround(std::ldexp(f, 14 + static_cast<int>(mantBits))));
        return std::min(mant, (1u << mantBits));  // rounding up reaches the smallest normal
    }
    auto mant = static_cast<uint32_t>(std::lround((m * 2.0f - 1.0f) * static_cast<float>(1u << mantBits)));
    if (mant == (1u << mantBits)) {
        mant = 0;
        ++e;
    }
    if (e >= 31) return maxBits;
    return (static_cast<uint32_t>(e) << mantBits) | mant;
}

uint32_t packRgb9e5(const float* rgb) {
    constexpr float kMax = 65408.0f;  // (2^9 - 1) / 2^9 * 2^16
    float c[3];
    for (int i = 0; i < 3; ++i) c[i] = std::isnan(rgb[i]) ? 0.0f : std::clamp(rgb[i], 0.0f, kMax);
    const float maxc = std::max({c[0], c[1], c[2]});
    int exp = 0;
    std::frexp(maxc, &exp);
    int sharedExp = std::max(-16, exp - 1) + 1 + 15;
    float denom = std::ldexp(1.0f, sharedExp - 15 - 9);
    if (std::lround(maxc / denom) == 512) {
        ++sharedExp;
        denom *= 2.0f;
    }
    uint32_t out = static_cast<uint32_t>(sharedExp) << 27;
    for (int i = 0; i < 3; ++i) out |= (static_cast<uint32_t>(std::lround(c[i] / denom)) & 0x1FF) << (9 * i);
    return out;
}

float unormValue(uint32_t v, uint32_t bits) { return static_cast<float>(v) / static_cast<float>((1ull << bits) - 1); }

float snormValue(int32_t v, uint32_t bits) {
    return std::max(static_cast<float>(v) / static_cast<float>((1u << (bits - 1)) - 1), -1.0f);
}

int components(GLenum format) {
    switch (format) {
        case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: return 1;
        case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: return 2;
        case GL_RGB: case GL_RGB_INTEGER: return 3;
        default: return 4;
    }
}

bool hasAlpha(GLenum format) {
    return format == GL_RGBA || format == GL_LUMINANCE_ALPHA || format == GL_ALPHA;
}

// One client pixel of (format, type) as a texel: components in RGBA order,
// with the defaults GL fills in (0, 0, 0, 1).
Texel decodeClient(GLenum format, GLenum type, const uint8_t* p) {
    Texel t;
    // Packed types carry all of their pixel's components.
    switch (type) {
        case GL_UNSIGNED_SHORT_5_6_5: {
            const uint16_t v = load<uint16_t>(p);
            t.f[0] = unormValue(v >> 11, 5);
            t.f[1] = unormValue((v >> 5) & 0x3F, 6);
            t.f[2] = unormValue(v & 0x1F, 5);
            return t;
        }
        case GL_UNSIGNED_SHORT_4_4_4_4: {
            const uint16_t v = load<uint16_t>(p);
            for (int c = 0; c < 4; ++c) t.f[c] = unormValue((v >> (12 - 4 * c)) & 0xF, 4);
            return t;
        }
        case GL_UNSIGNED_SHORT_5_5_5_1: {
            const uint16_t v = load<uint16_t>(p);
            for (int c = 0; c < 3; ++c) t.f[c] = unormValue((v >> (11 - 5 * c)) & 0x1F, 5);
            t.f[3] = static_cast<float>(v & 1);
            return t;
        }
        case GL_UNSIGNED_INT_2_10_10_10_REV: {
            const uint32_t v = load<uint32_t>(p);
            for (int c = 0; c < 3; ++c) {
                t.u[c] = (v >> (10 * c)) & 0x3FF;
                t.f[c] = unormValue(t.u[c], 10);
            }
            t.u[3] = v >> 30;
            t.f[3] = unormValue(t.u[3], 2);
            return t;
        }
        case GL_UNSIGNED_INT_10F_11F_11F_REV: {
            const uint32_t v = load<uint32_t>(p);
            t.f[0] = smallFloat(v & 0x7FF, 6);
            t.f[1] = smallFloat((v >> 11) & 0x7FF, 6);
            t.f[2] = smallFloat(v >> 22, 5);
            return t;
        }
        case GL_UNSIGNED_INT_5_9_9_9_REV: {
            const uint32_t v = load<uint32_t>(p);
            const float scale = std::ldexp(1.0f, static_cast<int>(v >> 27) - 15 - 9);
            for (int c = 0; c < 3; ++c) t.f[c] = static_cast<float>((v >> (9 * c)) & 0x1FF) * scale;
            return t;
        }
        default: break;
    }

    const int n = components(format);
    float f[4]{};
    int32_t iv[4]{};
    uint32_t uv[4]{};
    for (int c = 0; c < n; ++c) {
        switch (type) {
            case GL_UNSIGNED_BYTE: uv[c] = p[c]; iv[c] = p[c]; f[c] = unormValue(uv[c], 8); break;
            case GL_BYTE: iv[c] = load<int8_t>(p, c); uv[c] = iv[c]; f[c] = snormValue(iv[c], 8); break;
            case GL_UNSIGNED_SHORT: uv[c] = load<uint16_t>(p, c); iv[c] = uv[c]; f[c] = unormValue(uv[c], 16); break;
            case GL_SHORT: iv[c] = load<int16_t>(p, c); uv[c] = iv[c]; f[c] = snormValue(iv[c], 16); break;
            case GL_UNSIGNED_INT:
                uv[c] = load<uint32_t>(p, c);
                iv[c] = static_cast<int32_t>(uv[c]);
                f[c] = static_cast<float>(static_cast<double>(uv[c]) / 4294967295.0);
                break;
            case GL_INT: iv[c] = load<int32_t>(p, c); uv[c] = iv[c]; f[c] = static_cast<float>(iv[c]); break;
            case GL_HALF_FLOAT: f[c] = halfToFloat(load<uint16_t>(p, c)); break;
            case GL_FLOAT: f[c] = load<float>(p, c); break;
            default: break;
        }
    }
    auto put = [&](int dst, int src) {
        t.f[dst] = f[src];
        t.i[dst] = iv[src];
        t.u[dst] = uv[src];
    };
    switch (format) {
        case GL_LUMINANCE: put(0, 0); put(1, 0); put(2, 0); break;
        case GL_LUMINANCE_ALPHA: put(0, 0); put(1, 0); put(2, 0); put(3, 1); break;
        case GL_ALPHA: put(3, 0); break;
        default: for (int c = 0; c < n; ++c) put(c, c); break;
    }
    return t;
}

uint8_t unorm8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }
int8_t snorm8(float v) { return static_cast<int8_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 127.0f)); }

// Stored channel c of `t` under `tf`'s channel map: the decoded component,
// or the constant one (alpha) / zero.
struct Channel {
    float f;
    int32_t i;
    uint32_t u;
};
Channel channel(const TexFormat& tf, const Texel& t, int c) {
    const int src = tf.channels[static_cast<size_t>(c)];
    if (src < 0) return c == 3 ? Channel{1.0f, 1, 1} : Channel{0.0f, 0, 0};
    return {t.f[src], t.i[src], t.u[src]};
}

void encodeStorage(const TexFormat& tf, const Texel& t, uint8_t* dst) {
    auto n = [&](int count, auto write) {
        for (int c = 0; c < count; ++c) write(c, channel(tf, t, c));
    };
    switch (tf.format) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            n(static_cast<int>(colorTexelSize(tf.format)), [&](int c, Channel ch) { dst[c] = unorm8(ch.f); });
            break;
        case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8B8A8_SNORM:
            n(static_cast<int>(colorTexelSize(tf.format)), [&](int c, Channel ch) { store<int8_t>(dst, c, snorm8(ch.f)); });
            break;
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT:
            n(static_cast<int>(colorTexelSize(tf.format) / 2),
              [&](int c, Channel ch) { store<uint16_t>(dst, c, floatToHalf(ch.f)); });
            break;
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT:
            n(static_cast<int>(colorTexelSize(tf.format) / 4), [&](int c, Channel ch) { store<float>(dst, c, ch.f); });
            break;
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
            n(static_cast<int>(colorTexelSize(tf.format)), [&](int c, Channel ch) { dst[c] = static_cast<uint8_t>(ch.u); });
            break;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
            n(static_cast<int>(colorTexelSize(tf.format)), [&](int c, Channel ch) { store<int8_t>(dst, c, static_cast<int8_t>(ch.i)); });
            break;
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
            n(static_cast<int>(colorTexelSize(tf.format) / 2),
              [&](int c, Channel ch) { store<uint16_t>(dst, c, static_cast<uint16_t>(ch.u)); });
            break;
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
            n(static_cast<int>(colorTexelSize(tf.format) / 2),
              [&](int c, Channel ch) { store<int16_t>(dst, c, static_cast<int16_t>(ch.i)); });
            break;
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
            n(static_cast<int>(colorTexelSize(tf.format) / 4), [&](int c, Channel ch) { store<uint32_t>(dst, c, ch.u); });
            break;
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
            n(static_cast<int>(colorTexelSize(tf.format) / 4), [&](int c, Channel ch) { store<int32_t>(dst, c, ch.i); });
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {
            uint32_t v = 0;
            for (int c = 0; c < 4; ++c) {
                const float max = c == 3 ? 3.0f : 1023.0f;
                v |= static_cast<uint32_t>(std::lround(std::clamp(channel(tf, t, c).f, 0.0f, 1.0f) * max)) << (10 * c);
            }
            store<uint32_t>(dst, 0, v);
            break;
        }
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: {
            uint32_t v = 0;
            for (int c = 0; c < 4; ++c) v |= (channel(tf, t, c).u & (c == 3 ? 0x3u : 0x3FFu)) << (10 * c);
            store<uint32_t>(dst, 0, v);
            break;
        }
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            store<uint32_t>(dst, 0, toSmallFloat(t.f[0], 6) | toSmallFloat(t.f[1], 6) << 11 |
                                        toSmallFloat(t.f[2], 5) << 22);
            break;
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: store<uint32_t>(dst, 0, packRgb9e5(t.f)); break;
        default: break;
    }
}

// A client depth (and stencil) pixel: depth in f[0], stencil in u[1].
Texel decodeDepth(GLenum type, const uint8_t* p) {
    Texel t;
    switch (type) {
        case GL_UNSIGNED_SHORT: t.f[0] = unormValue(load<uint16_t>(p), 16); break;
        case GL_UNSIGNED_INT: t.f[0] = static_cast<float>(load<uint32_t>(p) / 4294967295.0); break;
        case GL_FLOAT: t.f[0] = load<float>(p); break;
        case GL_UNSIGNED_INT_24_8: {
            const uint32_t v = load<uint32_t>(p);
            t.f[0] = unormValue(v >> 8, 24);
            t.u[1] = v & 0xFF;
            break;
        }
        case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
            t.f[0] = load<float>(p);
            t.u[1] = load<uint32_t>(p, 1) & 0xFF;
            break;
        default: break;
    }
    return t;
}

void encodeDepth(VkFormat format, float depth, uint8_t* dst) {
    const float d = std::isnan(depth) ? 0.0f : std::clamp(depth, 0.0f, 1.0f);
    switch (format) {
        case VK_FORMAT_D16_UNORM: store<uint16_t>(dst, 0, static_cast<uint16_t>(std::lround(d * 65535.0f))); break;
        case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D24_UNORM_S8_UINT:
            store<uint32_t>(dst, 0, static_cast<uint32_t>(std::lround(static_cast<double>(d) * 16777215.0)));
            break;
        default: store<float>(dst, 0, d); break;
    }
}

// Whether a client pixel of (format, type) is already a stored texel of
// `tf`, byte for byte.
bool storedAsIs(const TexFormat& tf, GLenum format, GLenum type) {
    if (tf.alphaOne || tf.isDepthOrStencil()) return false;
    switch (tf.format) {
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            return type == GL_UNSIGNED_INT_2_10_10_10_REV;
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return type == GL_UNSIGNED_INT_10F_11F_11F_REV;
        case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return type == GL_UNSIGNED_INT_5_9_9_9_REV;
        default: break;
    }
    if (clientPixelSize(format, type) != colorTexelSize(tf.format)) return false;
    // Same size and the same component count: the same component type, as
    // the format tables pair them, except an unsigned client type into a
    // normalized float (or vice versa) of the same size.
    const bool f16 = tf.format == VK_FORMAT_R16_SFLOAT || tf.format == VK_FORMAT_R16G16_SFLOAT ||
                     tf.format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool f32 = tf.format == VK_FORMAT_R32_SFLOAT || tf.format == VK_FORMAT_R32G32_SFLOAT ||
                     tf.format == VK_FORMAT_R32G32B32A32_SFLOAT;
    if (f16) return type == GL_HALF_FLOAT;
    if (f32) return type == GL_FLOAT;
    return type != GL_HALF_FLOAT && type != GL_FLOAT;
}

size_t alignUp(size_t v, size_t a) { return a > 1 ? (v + a - 1) / a * a : v; }

} // namespace

uint16_t floatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const uint32_t absx = x & 0x7FFFFFFF;
    if (absx >= 0x7F800000) return static_cast<uint16_t>(sign | 0x7C00 | (absx > 0x7F800000 ? 0x200 : 0));
    if (absx >= 0x477FF000) return static_cast<uint16_t>(sign | 0x7C00);  // rounds past the largest half
    if (absx < 0x38800000) {  // a half denormal (or zero)
        const float a = std::fabs(f);
        return static_cast<uint16_t>(sign | static_cast<uint32_t>(std::nearbyint(std::ldexp(a, 24))));
    }
    // Round to nearest even on the 13 dropped mantissa bits.
    const uint32_t rounded = absx + 0xFFF + ((absx >> 13) & 1);
    return static_cast<uint16_t>(sign | ((rounded - 0x38000000) >> 13));
}

size_t unpackedSize(const UnpackState& state, GLenum format, GLenum type, uint32_t width, uint32_t height,
                    uint32_t depth) {
    const uint32_t pixel = clientPixelSize(format, type);
    if (pixel == 0 || width == 0 || height == 0 || depth == 0) return 0;
    const size_t rowLength = state.rowLength > 0 ? static_cast<size_t>(state.rowLength) : width;
    const size_t imageHeight = state.imageHeight > 0 ? static_cast<size_t>(state.imageHeight) : height;
    const size_t rowStride = alignUp(rowLength * pixel, static_cast<size_t>(state.alignment));
    const size_t imageStride = rowStride * imageHeight;
    return (static_cast<size_t>(state.skipImages) + depth - 1) * imageStride +
           (static_cast<size_t>(state.skipRows) + height - 1) * rowStride +
           (static_cast<size_t>(state.skipPixels) + width) * pixel;
}

void unpackPixels(const UnpackState& state, const void* srcIn, GLenum format, GLenum type, uint32_t width,
                  uint32_t height, uint32_t depth, const TexFormat& tf, uint8_t* dst, uint8_t* stencil) {
    const auto* src = static_cast<const uint8_t*>(srcIn);
    const uint32_t pixel = clientPixelSize(format, type);
    const size_t rowLength = state.rowLength > 0 ? static_cast<size_t>(state.rowLength) : width;
    const size_t imageHeight = state.imageHeight > 0 ? static_cast<size_t>(state.imageHeight) : height;
    const size_t rowStride = alignUp(rowLength * pixel, static_cast<size_t>(state.alignment));
    const size_t imageStride = rowStride * imageHeight;
    const uint8_t* base = src + static_cast<size_t>(state.skipImages) * imageStride +
                          static_cast<size_t>(state.skipRows) * rowStride +
                          static_cast<size_t>(state.skipPixels) * pixel;

    const bool depthStencil = tf.isDepthOrStencil();
    const uint32_t texel = depthStencil ? storageTexelSize(tf.format, VK_IMAGE_ASPECT_DEPTH_BIT)
                                        : colorTexelSize(tf.format);
    const bool premultiply = state.premultiplyAlpha && hasAlpha(format) && !tf.isInteger() && !depthStencil;
    const bool asIs = !premultiply && storedAsIs(tf, format, type);
    const size_t dstRow = static_cast<size_t>(width) * texel;

    for (uint32_t z = 0; z < depth; ++z) {
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* in = base + z * imageStride + y * rowStride;
            const size_t outRow = static_cast<size_t>(z) * height + (state.flipY ? height - 1 - y : y);
            uint8_t* out = dst + outRow * dstRow;
            if (asIs) {
                std::memcpy(out, in, dstRow);
                continue;
            }
            for (uint32_t x = 0; x < width; ++x, in += pixel, out += texel) {
                if (depthStencil) {
                    const Texel t = decodeDepth(type, in);
                    encodeDepth(tf.format, t.f[0], out);
                    if (stencil) stencil[outRow * width + x] = static_cast<uint8_t>(t.u[1]);
                    continue;
                }
                Texel t = decodeClient(format, type, in);
                if (premultiply) {
                    // In 8-bit terms when the data is 8-bit, as browsers do.
                    for (int c = 0; c < 3; ++c) {
                        t.f[c] = type == GL_UNSIGNED_BYTE ? static_cast<float>((in[c] * in[3] + 127) / 255) / 255.0f
                                                          : t.f[c] * t.f[3];
                    }
                    if (format == GL_LUMINANCE_ALPHA && type == GL_UNSIGNED_BYTE)
                        t.f[0] = t.f[1] = t.f[2] = static_cast<float>((in[0] * in[1] + 127) / 255) / 255.0f;
                }
                encodeStorage(tf, t, out);
            }
        }
    }
}

PackLayout packLayout(const PackState& state, GLenum format, GLenum type, uint32_t width) {
    PackLayout layout;
    layout.pixelSize = clientPixelSize(format, type);
    const size_t rowLength = state.rowLength > 0 ? static_cast<size_t>(state.rowLength) : width;
    layout.rowStride = alignUp(rowLength * layout.pixelSize, static_cast<size_t>(state.alignment));
    layout.offset = static_cast<size_t>(state.skipRows) * layout.rowStride +
                    static_cast<size_t>(state.skipPixels) * layout.pixelSize;
    return layout;
}

size_t packedSize(const PackState& state, GLenum format, GLenum type, uint32_t width, uint32_t height) {
    const PackLayout layout = packLayout(state, format, type, width);
    if (layout.pixelSize == 0 || width == 0 || height == 0) return 0;
    return layout.offset + (height - 1) * layout.rowStride + static_cast<size_t>(width) * layout.pixelSize;
}

void packTexel(VkFormat storage, const uint8_t* src, GLenum format, GLenum type, uint8_t* dst, bool alphaOne) {
    Texel t = decodeTexel(storage, src);
    if (alphaOne) {
        t.f[3] = 1.0f;
        t.i[3] = 1;
        t.u[3] = 1;
    }
    if (type == GL_UNSIGNED_INT_2_10_10_10_REV) {
        auto unorm = [&](int c, float max) {
            return static_cast<uint32_t>(std::lround(std::clamp(t.f[c], 0.0f, 1.0f) * max));
        };
        store<uint32_t>(dst, 0, unorm(0, 1023.0f) | unorm(1, 1023.0f) << 10 | unorm(2, 1023.0f) << 20 |
                                    unorm(3, 3.0f) << 30);
        return;
    }
    const int n = components(format);
    for (int c = 0; c < n; ++c) {
        switch (type) {
            case GL_UNSIGNED_BYTE: dst[c] = unorm8(t.f[c]); break;
            case GL_FLOAT: store<float>(dst, c, t.f[c]); break;
            case GL_HALF_FLOAT: store<uint16_t>(dst, c, floatToHalf(t.f[c])); break;
            case GL_INT: store<int32_t>(dst, c, t.i[c]); break;
            case GL_UNSIGNED_INT: store<uint32_t>(dst, c, t.u[c]); break;
            default: break;
        }
    }
}

} // namespace bro::webgl::vk
