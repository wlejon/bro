// GL texture formats on Vulkan: which (internalformat, format, type)
// combinations ES 3.0 allows, the Vulkan format each is stored in, and the
// compressed formats with the extensions that expose them.

#include "webgl/vulkan/webgl_vk_texformat.h"
#include "webgl/vulkan/webgl_vk_formats.h"

#include <initializer_list>

namespace bro::webgl::vk {

namespace {

constexpr VkComponentSwizzle R = VK_COMPONENT_SWIZZLE_R;
constexpr VkComponentSwizzle G = VK_COMPONENT_SWIZZLE_G;
constexpr VkComponentSwizzle ONE = VK_COMPONENT_SWIZZLE_ONE;
constexpr VkComponentSwizzle ZERO = VK_COMPONENT_SWIZZLE_ZERO;

// One row of ES 3.0 table 3.2 (sized) or 3.3 (unsized): an internal format,
// the client format it is specified with and the types that may carry it.
struct Combination {
    GLenum internalformat;
    GLenum format;
    std::initializer_list<GLenum> types;
};

const Combination kCombinations[] = {
    {GL_RGBA8, GL_RGBA, {GL_UNSIGNED_BYTE}},
    {GL_SRGB8_ALPHA8, GL_RGBA, {GL_UNSIGNED_BYTE}},
    {GL_RGBA8_SNORM, GL_RGBA, {GL_BYTE}},
    {GL_RGB5_A1, GL_RGBA, {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT_5_5_5_1, GL_UNSIGNED_INT_2_10_10_10_REV}},
    {GL_RGBA4, GL_RGBA, {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT_4_4_4_4}},
    {GL_RGB10_A2, GL_RGBA, {GL_UNSIGNED_INT_2_10_10_10_REV}},
    {GL_RGBA16F, GL_RGBA, {GL_HALF_FLOAT, GL_FLOAT}},
    {GL_RGBA32F, GL_RGBA, {GL_FLOAT}},
    {GL_RGBA8UI, GL_RGBA_INTEGER, {GL_UNSIGNED_BYTE}},
    {GL_RGBA8I, GL_RGBA_INTEGER, {GL_BYTE}},
    {GL_RGB10_A2UI, GL_RGBA_INTEGER, {GL_UNSIGNED_INT_2_10_10_10_REV}},
    {GL_RGBA16UI, GL_RGBA_INTEGER, {GL_UNSIGNED_SHORT}},
    {GL_RGBA16I, GL_RGBA_INTEGER, {GL_SHORT}},
    {GL_RGBA32UI, GL_RGBA_INTEGER, {GL_UNSIGNED_INT}},
    {GL_RGBA32I, GL_RGBA_INTEGER, {GL_INT}},
    {GL_RGB8, GL_RGB, {GL_UNSIGNED_BYTE}},
    {GL_SRGB8, GL_RGB, {GL_UNSIGNED_BYTE}},
    {GL_RGB565, GL_RGB, {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT_5_6_5}},
    {GL_RGB8_SNORM, GL_RGB, {GL_BYTE}},
    {GL_R11F_G11F_B10F, GL_RGB, {GL_UNSIGNED_INT_10F_11F_11F_REV, GL_HALF_FLOAT, GL_FLOAT}},
    {GL_RGB9_E5, GL_RGB, {GL_UNSIGNED_INT_5_9_9_9_REV, GL_HALF_FLOAT, GL_FLOAT}},
    {GL_RGB16F, GL_RGB, {GL_HALF_FLOAT, GL_FLOAT}},
    {GL_RGB32F, GL_RGB, {GL_FLOAT}},
    {GL_RGB8UI, GL_RGB_INTEGER, {GL_UNSIGNED_BYTE}},
    {GL_RGB8I, GL_RGB_INTEGER, {GL_BYTE}},
    {GL_RGB16UI, GL_RGB_INTEGER, {GL_UNSIGNED_SHORT}},
    {GL_RGB16I, GL_RGB_INTEGER, {GL_SHORT}},
    {GL_RGB32UI, GL_RGB_INTEGER, {GL_UNSIGNED_INT}},
    {GL_RGB32I, GL_RGB_INTEGER, {GL_INT}},
    {GL_RG8, GL_RG, {GL_UNSIGNED_BYTE}},
    {GL_RG8_SNORM, GL_RG, {GL_BYTE}},
    {GL_RG16F, GL_RG, {GL_HALF_FLOAT, GL_FLOAT}},
    {GL_RG32F, GL_RG, {GL_FLOAT}},
    {GL_RG8UI, GL_RG_INTEGER, {GL_UNSIGNED_BYTE}},
    {GL_RG8I, GL_RG_INTEGER, {GL_BYTE}},
    {GL_RG16UI, GL_RG_INTEGER, {GL_UNSIGNED_SHORT}},
    {GL_RG16I, GL_RG_INTEGER, {GL_SHORT}},
    {GL_RG32UI, GL_RG_INTEGER, {GL_UNSIGNED_INT}},
    {GL_RG32I, GL_RG_INTEGER, {GL_INT}},
    {GL_R8, GL_RED, {GL_UNSIGNED_BYTE}},
    {GL_R8_SNORM, GL_RED, {GL_BYTE}},
    {GL_R16F, GL_RED, {GL_HALF_FLOAT, GL_FLOAT}},
    {GL_R32F, GL_RED, {GL_FLOAT}},
    {GL_R8UI, GL_RED_INTEGER, {GL_UNSIGNED_BYTE}},
    {GL_R8I, GL_RED_INTEGER, {GL_BYTE}},
    {GL_R16UI, GL_RED_INTEGER, {GL_UNSIGNED_SHORT}},
    {GL_R16I, GL_RED_INTEGER, {GL_SHORT}},
    {GL_R32UI, GL_RED_INTEGER, {GL_UNSIGNED_INT}},
    {GL_R32I, GL_RED_INTEGER, {GL_INT}},
    {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, {GL_UNSIGNED_SHORT, GL_UNSIGNED_INT}},
    {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, {GL_UNSIGNED_INT}},
    {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, {GL_FLOAT}},
    {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, {GL_UNSIGNED_INT_24_8}},
    {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, {GL_FLOAT_32_UNSIGNED_INT_24_8_REV}},
};

// The unsized formats (table 3.3, plus the float types WebGL 1's
// OES_texture_float / _half_float allowed, and WEBGL_depth_texture's depth
// formats): the sized format each combination stands for.
GLenum unsizedFormat(GLenum internalformat, GLenum format, GLenum type) {
    if (internalformat != format) return 0;
    const bool f32 = type == GL_FLOAT, f16 = type == GL_HALF_FLOAT, u8 = type == GL_UNSIGNED_BYTE;
    switch (format) {
        case GL_RGBA:
            if (u8) return GL_RGBA8;
            if (type == GL_UNSIGNED_SHORT_4_4_4_4) return GL_RGBA4;
            if (type == GL_UNSIGNED_SHORT_5_5_5_1) return GL_RGB5_A1;
            return f32 ? GL_RGBA32F : f16 ? GL_RGBA16F : 0;
        case GL_RGB:
            if (u8) return GL_RGB8;
            if (type == GL_UNSIGNED_SHORT_5_6_5) return GL_RGB565;
            return f32 ? GL_RGB32F : f16 ? GL_RGB16F : 0;
        case GL_LUMINANCE_ALPHA:
        case GL_LUMINANCE:
        case GL_ALPHA: return u8 || f32 || f16 ? format : 0;
        case GL_DEPTH_COMPONENT:
            return type == GL_UNSIGNED_SHORT ? GL_DEPTH_COMPONENT16
                   : type == GL_UNSIGNED_INT ? GL_DEPTH_COMPONENT24 : 0;
        case GL_DEPTH_STENCIL: return type == GL_UNSIGNED_INT_24_8 ? GL_DEPTH24_STENCIL8 : 0;
        default: return 0;
    }
}

TexFormat color(GLenum internalformat, GLenum base, VkFormat format, TexKind kind = TexKind::Float) {
    TexFormat tf;
    tf.internalformat = internalformat;
    tf.baseFormat = base;
    tf.format = format;
    tf.kind = kind;
    return tf;
}

// An RGB format stored as RGBA: alpha written as one and read as one.
TexFormat rgb(GLenum internalformat, GLenum base, VkFormat format, TexKind kind = TexKind::Float) {
    TexFormat tf = color(internalformat, base, format, kind);
    tf.alphaOne = true;
    tf.channels = {0, 1, 2, -1};
    tf.swizzle.a = ONE;
    return tf;
}

bool sizedColorFormat(GLenum internalformat, TexFormat& tf) {
    using K = TexKind;
    switch (internalformat) {
        case GL_RGBA8: case GL_RGB5_A1: case GL_RGBA4:
            tf = color(internalformat, GL_RGBA, VK_FORMAT_R8G8B8A8_UNORM); return true;
        case GL_SRGB8_ALPHA8: tf = color(internalformat, GL_RGBA, VK_FORMAT_R8G8B8A8_SRGB); return true;
        case GL_RGBA8_SNORM: tf = color(internalformat, GL_RGBA, VK_FORMAT_R8G8B8A8_SNORM); return true;
        case GL_RGB10_A2: tf = color(internalformat, GL_RGBA, VK_FORMAT_A2B10G10R10_UNORM_PACK32); return true;
        case GL_RGBA16F: tf = color(internalformat, GL_RGBA, VK_FORMAT_R16G16B16A16_SFLOAT); return true;
        case GL_RGBA32F: tf = color(internalformat, GL_RGBA, VK_FORMAT_R32G32B32A32_SFLOAT); return true;
        case GL_RGBA8UI: tf = color(internalformat, GL_RGBA_INTEGER, VK_FORMAT_R8G8B8A8_UINT, K::Uint); return true;
        case GL_RGBA8I: tf = color(internalformat, GL_RGBA_INTEGER, VK_FORMAT_R8G8B8A8_SINT, K::Int); return true;
        case GL_RGB10_A2UI:
            tf = color(internalformat, GL_RGBA_INTEGER, VK_FORMAT_A2B10G10R10_UINT_PACK32, K::Uint); return true;
        case GL_RGBA16UI:
            tf = color(internalformat, GL_RGBA_INTEGER, VK_FORMAT_R16G16B16A16_UINT, K::Uint); return true;
        case GL_RGBA16I: tf = color(internalformat, GL_RGBA_INTEGER, VK_FORMAT_R16G16B16A16_SINT, K::Int); return true;
        case GL_RGBA32UI:
            tf = color(internalformat, GL_RGBA_INTEGER, VK_FORMAT_R32G32B32A32_UINT, K::Uint); return true;
        case GL_RGBA32I: tf = color(internalformat, GL_RGBA_INTEGER, VK_FORMAT_R32G32B32A32_SINT, K::Int); return true;
        case GL_RGB8: case GL_RGB565: tf = rgb(internalformat, GL_RGB, VK_FORMAT_R8G8B8A8_UNORM); return true;
        case GL_SRGB8: tf = rgb(internalformat, GL_RGB, VK_FORMAT_R8G8B8A8_SRGB); return true;
        case GL_RGB8_SNORM: tf = rgb(internalformat, GL_RGB, VK_FORMAT_R8G8B8A8_SNORM); return true;
        case GL_R11F_G11F_B10F:
            tf = color(internalformat, GL_RGB, VK_FORMAT_B10G11R11_UFLOAT_PACK32); return true;
        case GL_RGB9_E5: tf = color(internalformat, GL_RGB, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32); return true;
        case GL_RGB16F: tf = rgb(internalformat, GL_RGB, VK_FORMAT_R16G16B16A16_SFLOAT); return true;
        case GL_RGB32F: tf = rgb(internalformat, GL_RGB, VK_FORMAT_R32G32B32A32_SFLOAT); return true;
        case GL_RGB8UI: tf = rgb(internalformat, GL_RGB_INTEGER, VK_FORMAT_R8G8B8A8_UINT, K::Uint); return true;
        case GL_RGB8I: tf = rgb(internalformat, GL_RGB_INTEGER, VK_FORMAT_R8G8B8A8_SINT, K::Int); return true;
        case GL_RGB16UI: tf = rgb(internalformat, GL_RGB_INTEGER, VK_FORMAT_R16G16B16A16_UINT, K::Uint); return true;
        case GL_RGB16I: tf = rgb(internalformat, GL_RGB_INTEGER, VK_FORMAT_R16G16B16A16_SINT, K::Int); return true;
        case GL_RGB32UI: tf = rgb(internalformat, GL_RGB_INTEGER, VK_FORMAT_R32G32B32A32_UINT, K::Uint); return true;
        case GL_RGB32I: tf = rgb(internalformat, GL_RGB_INTEGER, VK_FORMAT_R32G32B32A32_SINT, K::Int); return true;
        case GL_RG8: tf = color(internalformat, GL_RG, VK_FORMAT_R8G8_UNORM); return true;
        case GL_RG8_SNORM: tf = color(internalformat, GL_RG, VK_FORMAT_R8G8_SNORM); return true;
        case GL_RG16F: tf = color(internalformat, GL_RG, VK_FORMAT_R16G16_SFLOAT); return true;
        case GL_RG32F: tf = color(internalformat, GL_RG, VK_FORMAT_R32G32_SFLOAT); return true;
        case GL_RG8UI: tf = color(internalformat, GL_RG_INTEGER, VK_FORMAT_R8G8_UINT, K::Uint); return true;
        case GL_RG8I: tf = color(internalformat, GL_RG_INTEGER, VK_FORMAT_R8G8_SINT, K::Int); return true;
        case GL_RG16UI: tf = color(internalformat, GL_RG_INTEGER, VK_FORMAT_R16G16_UINT, K::Uint); return true;
        case GL_RG16I: tf = color(internalformat, GL_RG_INTEGER, VK_FORMAT_R16G16_SINT, K::Int); return true;
        case GL_RG32UI: tf = color(internalformat, GL_RG_INTEGER, VK_FORMAT_R32G32_UINT, K::Uint); return true;
        case GL_RG32I: tf = color(internalformat, GL_RG_INTEGER, VK_FORMAT_R32G32_SINT, K::Int); return true;
        case GL_R8: tf = color(internalformat, GL_RED, VK_FORMAT_R8_UNORM); return true;
        case GL_R8_SNORM: tf = color(internalformat, GL_RED, VK_FORMAT_R8_SNORM); return true;
        case GL_R16F: tf = color(internalformat, GL_RED, VK_FORMAT_R16_SFLOAT); return true;
        case GL_R32F: tf = color(internalformat, GL_RED, VK_FORMAT_R32_SFLOAT); return true;
        case GL_R8UI: tf = color(internalformat, GL_RED_INTEGER, VK_FORMAT_R8_UINT, K::Uint); return true;
        case GL_R8I: tf = color(internalformat, GL_RED_INTEGER, VK_FORMAT_R8_SINT, K::Int); return true;
        case GL_R16UI: tf = color(internalformat, GL_RED_INTEGER, VK_FORMAT_R16_UINT, K::Uint); return true;
        case GL_R16I: tf = color(internalformat, GL_RED_INTEGER, VK_FORMAT_R16_SINT, K::Int); return true;
        case GL_R32UI: tf = color(internalformat, GL_RED_INTEGER, VK_FORMAT_R32_UINT, K::Uint); return true;
        case GL_R32I: tf = color(internalformat, GL_RED_INTEGER, VK_FORMAT_R32_SINT, K::Int); return true;
        default: return false;
    }
}

// LUMINANCE / ALPHA / LUMINANCE_ALPHA: one or two channels and a swizzle.
// The storage precision follows the client type.
TexFormat luminanceAlpha(GLenum base, GLenum type) {
    const bool f32 = type == GL_FLOAT, f16 = type == GL_HALF_FLOAT;
    TexFormat tf;
    tf.internalformat = base;
    tf.baseFormat = base;
    tf.sized = false;
    if (base == GL_LUMINANCE_ALPHA) {
        tf.format = f32 ? VK_FORMAT_R32G32_SFLOAT : f16 ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_R8G8_UNORM;
        tf.channels = {0, 3, -1, -1};
        tf.swizzle = {R, R, R, G};
    } else {
        tf.format = f32 ? VK_FORMAT_R32_SFLOAT : f16 ? VK_FORMAT_R16_SFLOAT : VK_FORMAT_R8_UNORM;
        if (base == GL_ALPHA) {
            tf.channels = {3, -1, -1, -1};
            tf.swizzle = {ZERO, ZERO, ZERO, R};
        } else {
            tf.channels = {0, -1, -1, -1};
            tf.swizzle = {R, R, R, ONE};
        }
    }
    return tf;
}

bool depthFormat(VkPhysicalDevice device, GLenum internalformat, TexFormat& tf) {
    const VkFormat format = depthStencilFormat(device, internalformat);
    if (format == VK_FORMAT_UNDEFINED || internalformat == GL_STENCIL_INDEX8 || internalformat == GL_DEPTH_STENCIL)
        return false;
    tf = TexFormat{};
    tf.internalformat = internalformat;
    tf.format = format;
    const bool stencil = internalformat == GL_DEPTH24_STENCIL8 || internalformat == GL_DEPTH32F_STENCIL8;
    tf.baseFormat = stencil ? GL_DEPTH_STENCIL : GL_DEPTH_COMPONENT;
    tf.kind = stencil ? TexKind::DepthStencil : TexKind::Depth;
    return true;
}

// --- Compressed formats ---

struct Compressed {
    GLenum gl;
    VkFormat vk;
    uint8_t blockWidth, blockHeight, blockBytes;
};

const Compressed kCompressed[] = {
    {GL_COMPRESSED_RGB_S3TC_DXT1_EXT, VK_FORMAT_BC1_RGB_UNORM_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, VK_FORMAT_BC2_UNORM_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, VK_FORMAT_BC3_UNORM_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_SRGB_S3TC_DXT1_EXT, VK_FORMAT_BC1_RGB_SRGB_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, VK_FORMAT_BC2_SRGB_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, VK_FORMAT_BC3_SRGB_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_RED_RGTC1_EXT, VK_FORMAT_BC4_UNORM_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_SIGNED_RED_RGTC1_EXT, VK_FORMAT_BC4_SNORM_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_RED_GREEN_RGTC2_EXT, VK_FORMAT_BC5_UNORM_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT, VK_FORMAT_BC5_SNORM_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_RGBA_BPTC_UNORM_EXT, VK_FORMAT_BC7_UNORM_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT, VK_FORMAT_BC7_SRGB_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_EXT, VK_FORMAT_BC6H_SFLOAT_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT, VK_FORMAT_BC6H_UFLOAT_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_R11_EAC, VK_FORMAT_EAC_R11_UNORM_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_SIGNED_R11_EAC, VK_FORMAT_EAC_R11_SNORM_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_RG11_EAC, VK_FORMAT_EAC_R11G11_UNORM_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_SIGNED_RG11_EAC, VK_FORMAT_EAC_R11G11_SNORM_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_RGB8_ETC2, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_SRGB8_ETC2, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2, VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK, 4, 4, 8},
    {GL_COMPRESSED_RGBA8_ETC2_EAC, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 4, 4, 16},
    {GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, 4, 4, 16},
    // ETC1 is a subset of ETC2.
    {GL_ETC1_RGB8_OES, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 4, 4, 8},
};

// ASTC: the GL enums of each block size are consecutive, as are Vulkan's
// UNORM / SRGB pairs, in the same order.
constexpr uint8_t kAstcBlocks[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6},
                                        {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

constexpr GLenum kS3tc[] = {0x83F0, 0x83F1, 0x83F2, 0x83F3};
constexpr GLenum kS3tcSrgb[] = {0x8C4C, 0x8C4D, 0x8C4E, 0x8C4F};
constexpr GLenum kRgtc[] = {0x8DBB, 0x8DBC, 0x8DBD, 0x8DBE};
constexpr GLenum kBptc[] = {0x8E8C, 0x8E8D, 0x8E8E, 0x8E8F};
constexpr GLenum kEtc[] = {0x9270, 0x9271, 0x9272, 0x9273, 0x9274, 0x9275, 0x9276, 0x9277, 0x9278, 0x9279};
constexpr GLenum kEtc1[] = {0x8D64};
constexpr GLenum kAstc[] = {0x93B0, 0x93B1, 0x93B2, 0x93B3, 0x93B4, 0x93B5, 0x93B6, 0x93B7, 0x93B8, 0x93B9,
                            0x93BA, 0x93BB, 0x93BC, 0x93BD, 0x93D0, 0x93D1, 0x93D2, 0x93D3, 0x93D4, 0x93D5,
                            0x93D6, 0x93D7, 0x93D8, 0x93D9, 0x93DA, 0x93DB, 0x93DC, 0x93DD};

const CompressedExtension kExtensions[] = {
    {"WEBGL_compressed_texture_s3tc", kS3tc, std::size(kS3tc)},
    {"WEBGL_compressed_texture_s3tc_srgb", kS3tcSrgb, std::size(kS3tcSrgb)},
    {"EXT_texture_compression_rgtc", kRgtc, std::size(kRgtc)},
    {"EXT_texture_compression_bptc", kBptc, std::size(kBptc)},
    {"WEBGL_compressed_texture_etc", kEtc, std::size(kEtc)},
    {"WEBGL_compressed_texture_etc1", kEtc1, std::size(kEtc1)},
    {"WEBGL_compressed_texture_astc", kAstc, std::size(kAstc)},
};

bool supportsSampling(VkPhysicalDevice device, VkFormat format) {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(device, format, &props);
    constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (props.optimalTilingFeatures & kNeeded) == kNeeded;
}

} // namespace

bool resolveSizedFormat(VkPhysicalDevice device, GLenum internalformat, TexFormat& out) {
    if (sizedColorFormat(internalformat, out)) return true;
    return depthFormat(device, internalformat, out);
}

bool resolveTexFormat(VkPhysicalDevice device, GLint internalformatIn, GLenum format, GLenum type, TexFormat& out,
                      GLenum& error) {
    const auto internalformat = static_cast<GLenum>(internalformatIn);
    if (clientPixelSize(format, type) == 0) {
        error = GL_INVALID_ENUM;
        return false;
    }
    if (const GLenum sized = unsizedFormat(internalformat, format, type)) {
        if (sized == GL_LUMINANCE || sized == GL_LUMINANCE_ALPHA || sized == GL_ALPHA) {
            out = luminanceAlpha(sized, type);
            return true;
        }
        if (!resolveSizedFormat(device, sized, out)) {
            error = GL_INVALID_OPERATION;
            return false;
        }
        out.sized = false;
        return true;
    }
    bool known = false;
    for (const Combination& c : kCombinations) {
        if (c.internalformat != internalformat) continue;
        known = true;
        if (c.format != format) continue;
        for (GLenum t : c.types) {
            if (t != type) continue;
            if (resolveSizedFormat(device, internalformat, out)) return true;
            error = GL_INVALID_OPERATION;  // no storage for it on this device
            return false;
        }
    }
    // An internal format the tables do not have is INVALID_VALUE; a known one
    // with the wrong format or type, INVALID_OPERATION.
    const bool unsizedName = internalformat == GL_RGBA || internalformat == GL_RGB ||
                             internalformat == GL_LUMINANCE || internalformat == GL_LUMINANCE_ALPHA ||
                             internalformat == GL_ALPHA || internalformat == GL_DEPTH_COMPONENT ||
                             internalformat == GL_DEPTH_STENCIL;
    error = known || unsizedName ? GL_INVALID_OPERATION : GL_INVALID_VALUE;
    return false;
}

bool clientFormatCompatible(VkPhysicalDevice device, const TexFormat& tf, GLenum format, GLenum type) {
    TexFormat other;
    GLenum error = GL_NO_ERROR;
    if (!tf.sized) {
        // An unsized texture takes its own format and any type of it.
        if (format != tf.baseFormat) return false;
        return resolveTexFormat(device, static_cast<GLint>(format), format, type, other, error) &&
               other.format == tf.format;
    }
    return resolveTexFormat(device, static_cast<GLint>(tf.internalformat), format, type, other, error);
}

bool resolveCompressedFormat(GLenum internalformat, TexFormat& out) {
    out = TexFormat{};
    out.internalformat = internalformat;
    out.baseFormat = GL_RGBA;
    out.compressed = true;
    for (const Compressed& c : kCompressed) {
        if (c.gl != internalformat) continue;
        out.format = c.vk;
        out.blockWidth = c.blockWidth;
        out.blockHeight = c.blockHeight;
        out.blockBytes = c.blockBytes;
        return true;
    }
    for (int srgb = 0; srgb < 2; ++srgb) {
        const GLenum first = srgb ? 0x93D0u : 0x93B0u;
        if (internalformat < first || internalformat >= first + 14) continue;
        const uint32_t i = internalformat - first;
        out.format = static_cast<VkFormat>(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * i + srgb);
        out.blockWidth = kAstcBlocks[i][0];
        out.blockHeight = kAstcBlocks[i][1];
        out.blockBytes = 16;
        return true;
    }
    return false;
}

size_t compressedImageSize(const TexFormat& tf, uint32_t width, uint32_t height, uint32_t depth) {
    if (!tf.compressed || tf.blockWidth == 0 || tf.blockHeight == 0) return 0;
    const size_t bw = (width + tf.blockWidth - 1) / tf.blockWidth;
    const size_t bh = (height + tf.blockHeight - 1) / tf.blockHeight;
    return bw * bh * depth * tf.blockBytes;
}

size_t compressedExtensions(const CompressedExtension*& out) {
    out = kExtensions;
    return std::size(kExtensions);
}

bool compressedExtensionSupported(VkPhysicalDevice device, const VkPhysicalDeviceFeatures& features,
                                  const CompressedExtension& ext) {
    for (size_t i = 0; i < ext.count; ++i) {
        TexFormat tf;
        if (!resolveCompressedFormat(ext.formats[i], tf)) return false;
        const bool bc = tf.format >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && tf.format <= VK_FORMAT_BC7_SRGB_BLOCK;
        const bool etc = tf.format >= VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK && tf.format <= VK_FORMAT_EAC_R11G11_SNORM_BLOCK;
        const bool enabled = bc ? features.textureCompressionBC
                             : etc ? features.textureCompressionETC2 : features.textureCompressionASTC_LDR;
        if (!enabled || !supportsSampling(device, tf.format)) return false;
    }
    return true;
}

uint32_t clientPixelSize(GLenum format, GLenum type) {
    uint32_t components = 0;
    switch (format) {
        case GL_RED: case GL_RED_INTEGER: case GL_ALPHA: case GL_LUMINANCE: case GL_DEPTH_COMPONENT:
            components = 1;
            break;
        case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: components = 2; break;
        case GL_RGB: case GL_RGB_INTEGER: components = 3; break;
        case GL_RGBA: case GL_RGBA_INTEGER: components = 4; break;
        case GL_DEPTH_STENCIL:
            return type == GL_UNSIGNED_INT_24_8 ? 4 : type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV ? 8 : 0;
        default: return 0;
    }
    switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: return components;
        case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: return components * 2;
        case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return components * 4;
        case GL_UNSIGNED_SHORT_5_6_5: return format == GL_RGB ? 2 : 0;
        case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return format == GL_RGBA ? 2 : 0;
        case GL_UNSIGNED_INT_2_10_10_10_REV: return format == GL_RGBA || format == GL_RGBA_INTEGER ? 4 : 0;
        case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV: return format == GL_RGB ? 4 : 0;
        default: return 0;
    }
}

uint32_t storageTexelSize(VkFormat format, VkImageAspectFlags aspect) {
    if (aspect == VK_IMAGE_ASPECT_STENCIL_BIT) return 1;
    if (aspect == VK_IMAGE_ASPECT_DEPTH_BIT) return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D16_UNORM_S8_UINT ? 2 : 4;
    return colorTexelSize(format);
}

GLenum defaultInternalFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB: return GL_SRGB8_ALPHA8;
        case VK_FORMAT_D16_UNORM: return GL_DEPTH_COMPONENT16;
        case VK_FORMAT_X8_D24_UNORM_PACK32: return GL_DEPTH_COMPONENT24;
        case VK_FORMAT_D32_SFLOAT: return GL_DEPTH_COMPONENT32F;
        case VK_FORMAT_D24_UNORM_S8_UINT: return GL_DEPTH24_STENCIL8;
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return GL_DEPTH32F_STENCIL8;
        case VK_FORMAT_S8_UINT: return GL_STENCIL_INDEX8;
        default: return GL_RGBA8;
    }
}

} // namespace bro::webgl::vk
