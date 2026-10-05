#include "webgl/vulkan/webgl_vk_formats.h"

#include <cmath>
#include <cstring>

namespace bro::webgl::vk {

namespace {

bool supportsDepthAttachment(VkPhysicalDevice device, VkFormat format) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(device, format, &props);
    return (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

// The first of `candidates` the device can render depth / stencil into.
template <size_t N>
VkFormat firstSupported(VkPhysicalDevice device, const VkFormat (&candidates)[N]) {
    for (VkFormat f : candidates)
        if (supportsDepthAttachment(device, f)) return f;
    return candidates[N - 1];
}

} // namespace

VkFormat depthStencilFormat(VkPhysicalDevice device, GLenum internalformat) {
    // Each list prefers the exact format and falls back to one with at least
    // the bits asked for; D32_SFLOAT and D32_SFLOAT_S8_UINT are the ones
    // every device has (or, for the latter, D24S8 is).
    static constexpr VkFormat kDepth16[] = {VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT};
    static constexpr VkFormat kDepth24[] = {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT};
    static constexpr VkFormat kDepth32F[] = {VK_FORMAT_D32_SFLOAT};
    static constexpr VkFormat kDepth24Stencil8[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    static constexpr VkFormat kDepth32FStencil8[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT};
    static constexpr VkFormat kStencil8[] = {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
                                             VK_FORMAT_D32_SFLOAT_S8_UINT};
    switch (internalformat) {
        case GL_DEPTH_COMPONENT16: return firstSupported(device, kDepth16);
        case GL_DEPTH_COMPONENT24: return firstSupported(device, kDepth24);
        case GL_DEPTH_COMPONENT32F: return firstSupported(device, kDepth32F);
        case GL_DEPTH_STENCIL:
        case GL_DEPTH24_STENCIL8: return firstSupported(device, kDepth24Stencil8);
        case GL_DEPTH32F_STENCIL8: return firstSupported(device, kDepth32FStencil8);
        case GL_STENCIL_INDEX8: return firstSupported(device, kStencil8);
        default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat colorRenderableFormat(GLenum internalformat) {
    switch (internalformat) {
        // GL lets an implementation store these with more bits than asked.
        case GL_RGBA4:
        case GL_RGB5_A1:
        case GL_RGB565:
        case GL_RGB8:
        case GL_RGBA8: return VK_FORMAT_R8G8B8A8_UNORM;
        case GL_SRGB8_ALPHA8: return VK_FORMAT_R8G8B8A8_SRGB;
        case GL_R8: return VK_FORMAT_R8_UNORM;
        case GL_RG8: return VK_FORMAT_R8G8_UNORM;
        case GL_RGB10_A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case GL_R16F: return VK_FORMAT_R16_SFLOAT;
        case GL_RG16F: return VK_FORMAT_R16G16_SFLOAT;
        case GL_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case GL_R32F: return VK_FORMAT_R32_SFLOAT;
        case GL_RG32F: return VK_FORMAT_R32G32_SFLOAT;
        case GL_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case GL_R11F_G11F_B10F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 0x8231: return VK_FORMAT_R8_SINT;           // R8I
        case 0x8232: return VK_FORMAT_R8_UINT;           // R8UI
        case 0x8233: return VK_FORMAT_R16_SINT;          // R16I
        case 0x8234: return VK_FORMAT_R16_UINT;          // R16UI
        case 0x8235: return VK_FORMAT_R32_SINT;          // R32I
        case 0x8236: return VK_FORMAT_R32_UINT;          // R32UI
        case 0x8237: return VK_FORMAT_R8G8_SINT;         // RG8I
        case 0x8238: return VK_FORMAT_R8G8_UINT;         // RG8UI
        case 0x8239: return VK_FORMAT_R16G16_SINT;       // RG16I
        case 0x823A: return VK_FORMAT_R16G16_UINT;       // RG16UI
        case 0x823B: return VK_FORMAT_R32G32_SINT;       // RG32I
        case 0x823C: return VK_FORMAT_R32G32_UINT;       // RG32UI
        case 0x8D8E: return VK_FORMAT_R8G8B8A8_SINT;     // RGBA8I
        case 0x8D7C: return VK_FORMAT_R8G8B8A8_UINT;     // RGBA8UI
        case 0x8D88: return VK_FORMAT_R16G16B16A16_SINT; // RGBA16I
        case 0x8D76: return VK_FORMAT_R16G16B16A16_UINT; // RGBA16UI
        case 0x8D82: return VK_FORMAT_R32G32B32A32_SINT; // RGBA32I
        case 0x8D70: return VK_FORMAT_R32G32B32A32_UINT; // RGBA32UI
        case 0x906F: return VK_FORMAT_A2B10G10R10_UINT_PACK32;  // RGB10_A2UI
        default: return VK_FORMAT_UNDEFINED;
    }
}

FormatBits formatBits(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_UINT: return {8, 8, 8, 8, 0, 0};
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_UINT: return {8, 0, 0, 0, 0, 0};
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_UINT: return {8, 8, 0, 0, 0, 0};
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: return {10, 10, 10, 2, 0, 0};
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_UINT: return {16, 0, 0, 0, 0, 0};
        case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_UINT:
            return {16, 16, 0, 0, 0, 0};
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_UINT:
            return {16, 16, 16, 16, 0, 0};
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_UINT: return {32, 0, 0, 0, 0, 0};
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_UINT:
            return {32, 32, 0, 0, 0, 0};
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_R32G32B32A32_UINT:
            return {32, 32, 32, 32, 0, 0};
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return {11, 11, 10, 0, 0, 0};
        case VK_FORMAT_D16_UNORM: return {0, 0, 0, 0, 16, 0};
        case VK_FORMAT_X8_D24_UNORM_PACK32: return {0, 0, 0, 0, 24, 0};
        case VK_FORMAT_D32_SFLOAT: return {0, 0, 0, 0, 32, 0};
        case VK_FORMAT_D24_UNORM_S8_UINT: return {0, 0, 0, 0, 24, 8};
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return {0, 0, 0, 0, 32, 8};
        case VK_FORMAT_S8_UINT: return {0, 0, 0, 0, 0, 8};
        default: return {};
    }
}

bool isSignedIntegerFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
            return true;
        default: return false;
    }
}

bool isIntegerFormat(VkFormat format) {
    if (isSignedIntegerFormat(format)) return true;
    switch (format) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT:
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            return true;
        default: return false;
    }
}

uint32_t colorTexelSize(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8_UINT: return 1;
        case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8_UINT:
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R16_UINT: return 2;
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16_UINT:
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32_UINT: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R16G16B16A16_UINT:
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32_UINT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_SINT:
        case VK_FORMAT_R32G32B32A32_UINT: return 16;
        default: return 0;
    }
}

namespace {

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    float v;
    if (exp == 0) v = std::ldexp(static_cast<float>(mant), -24);
    else if (exp == 31) v = mant ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(mant | 0x400), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

// An unsigned float with `mantBits` mantissa bits and a 5-bit exponent
// (the 11- and 10-bit channels of B10G11R11).
float smallFloat(uint32_t bits, uint32_t mantBits) {
    const uint32_t exp = bits >> mantBits, mant = bits & ((1u << mantBits) - 1);
    if (exp == 0) return std::ldexp(static_cast<float>(mant), -14 - static_cast<int>(mantBits));
    if (exp == 31) return mant ? NAN : INFINITY;
    return std::ldexp(static_cast<float>(mant | (1u << mantBits)), static_cast<int>(exp) - 15 - static_cast<int>(mantBits));
}

template <typename T>
T load(const uint8_t* p, int index) {
    T v;
    std::memcpy(&v, p + index * sizeof(T), sizeof(T));
    return v;
}

} // namespace

Texel decodeTexel(VkFormat format, const uint8_t* src) {
    Texel t;
    auto channels = [&](int n, auto read) {
        for (int c = 0; c < n; ++c) read(c);
    };
    switch (format) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB: {
            const int n = format == VK_FORMAT_R8_UNORM ? 1 : format == VK_FORMAT_R8G8_UNORM ? 2 : 4;
            channels(n, [&](int c) { t.f[c] = src[c] / 255.0f; });
            break;
        }
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: {
            const int n = format == VK_FORMAT_R16_SFLOAT ? 1 : format == VK_FORMAT_R16G16_SFLOAT ? 2 : 4;
            channels(n, [&](int c) { t.f[c] = halfToFloat(load<uint16_t>(src, c)); });
            break;
        }
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: {
            const int n = format == VK_FORMAT_R32_SFLOAT ? 1 : format == VK_FORMAT_R32G32_SFLOAT ? 2 : 4;
            channels(n, [&](int c) { t.f[c] = load<float>(src, c); });
            break;
        }
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32: {
            const uint32_t v = load<uint32_t>(src, 0);
            const uint32_t parts[4] = {v & 0x3FF, (v >> 10) & 0x3FF, (v >> 20) & 0x3FF, v >> 30};
            for (int c = 0; c < 4; ++c) {
                t.u[c] = parts[c];
                t.f[c] = static_cast<float>(parts[c]) / (c == 3 ? 3.0f : 1023.0f);
            }
            break;
        }
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {
            const uint32_t v = load<uint32_t>(src, 0);
            t.f[0] = smallFloat(v & 0x7FF, 6);
            t.f[1] = smallFloat((v >> 11) & 0x7FF, 6);
            t.f[2] = smallFloat(v >> 22, 5);
            break;
        }
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: {
            const int n = format == VK_FORMAT_R8_SINT ? 1 : format == VK_FORMAT_R8G8_SINT ? 2 : 4;
            channels(n, [&](int c) { t.i[c] = load<int8_t>(src, c); });
            break;
        }
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: {
            const int n = format == VK_FORMAT_R16_SINT ? 1 : format == VK_FORMAT_R16G16_SINT ? 2 : 4;
            channels(n, [&](int c) { t.i[c] = load<int16_t>(src, c); });
            break;
        }
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: {
            const int n = format == VK_FORMAT_R32_SINT ? 1 : format == VK_FORMAT_R32G32_SINT ? 2 : 4;
            channels(n, [&](int c) { t.i[c] = load<int32_t>(src, c); });
            break;
        }
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: {
            const int n = format == VK_FORMAT_R8_UINT ? 1 : format == VK_FORMAT_R8G8_UINT ? 2 : 4;
            channels(n, [&](int c) { t.u[c] = src[c]; });
            break;
        }
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: {
            const int n = format == VK_FORMAT_R16_UINT ? 1 : format == VK_FORMAT_R16G16_UINT ? 2 : 4;
            channels(n, [&](int c) { t.u[c] = load<uint16_t>(src, c); });
            break;
        }
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT: {
            const int n = format == VK_FORMAT_R32_UINT ? 1 : format == VK_FORMAT_R32G32_UINT ? 2 : 4;
            channels(n, [&](int c) { t.u[c] = load<uint32_t>(src, c); });
            break;
        }
        default: break;
    }
    return t;
}

} // namespace bro::webgl::vk
