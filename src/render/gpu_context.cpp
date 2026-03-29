#include "render/gpu_context.h"
#include "platform/sdl_window.h"
#include "util/log.h"

#include <SDL3/SDL.h>
#include <cstring>
#include <stdexcept>

// Auto-generated compiled shader bytecode
#include "compiled_shaders.h"

namespace bro::render {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GPUContext::GPUContext(platform::Window& window, bool debug) {
    sdlWindow_ = window.getSDLWindow();

    device_ = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_DXBC,
        debug,
        "direct3d12"
    );
    if (!device_) {
        LOG_ERROR("Failed to create GPU device: %s", SDL_GetError());
        throw std::runtime_error("SDL_CreateGPUDevice failed");
    }

    const char* driver = SDL_GetGPUDeviceDriver(device_);
    LOG_INFO("GPU device created (driver: %s)", driver ? driver : "unknown");

    if (!SDL_ClaimWindowForGPUDevice(device_, sdlWindow_)) {
        LOG_ERROR("Failed to claim window for GPU: %s", SDL_GetError());
        throw std::runtime_error("SDL_ClaimWindowForGPUDevice failed");
    }

    // Query the swapchain texture format
    swapchainFormat_ = SDL_GetGPUSwapchainTextureFormat(device_, sdlWindow_);
    LOG_INFO("Swapchain format: %d", (int)swapchainFormat_);

    // Create sampler
    SDL_GPUSamplerCreateInfo samplerInfo = {};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    linearSampler_ = SDL_CreateGPUSampler(device_, &samplerInfo);
    if (!linearSampler_) {
        throw std::runtime_error("Failed to create GPU sampler");
    }

    createPipelines();
}

GPUContext::~GPUContext() {
    if (device_) {
        SDL_WaitForGPUIdle(device_);
        if (colorPipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, colorPipeline_);
        if (texturePipeline_) SDL_ReleaseGPUGraphicsPipeline(device_, texturePipeline_);
        if (linearSampler_) SDL_ReleaseGPUSampler(device_, linearSampler_);
        if (transferBuf_) SDL_ReleaseGPUTransferBuffer(device_, transferBuf_);
        SDL_ReleaseWindowFromGPUDevice(device_, sdlWindow_);
        SDL_DestroyGPUDevice(device_);
    }
}

// ---------------------------------------------------------------------------
// Shader loading
// ---------------------------------------------------------------------------

SDL_GPUShader* GPUContext::createShader(SDL_GPUShaderStage stage,
                                         const uint8_t* code, size_t codeSize,
                                         uint32_t numSamplers,
                                         uint32_t numUniformBuffers) {
    SDL_GPUShaderCreateInfo info = {};
    info.code = code;
    info.code_size = codeSize;
    info.entrypoint = "main";
    info.format = SDL_GPU_SHADERFORMAT_DXBC;
    info.stage = stage;
    info.num_samplers = numSamplers;
    info.num_uniform_buffers = numUniformBuffers;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device_, &info);
    if (!shader) {
        LOG_ERROR("Failed to create shader: %s", SDL_GetError());
    }
    return shader;
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void GPUContext::createPipelines() {
    // --- Color pipeline ---
    {
        SDL_GPUShader* vs = createShader(SDL_GPU_SHADERSTAGE_VERTEX,
            shaders::color_vert, shaders::color_vert_size, 0, 1);
        SDL_GPUShader* fs = createShader(SDL_GPU_SHADERSTAGE_FRAGMENT,
            shaders::color_frag, shaders::color_frag_size, 0, 0);

        SDL_GPUColorTargetDescription colorDesc = {};
        colorDesc.format = swapchainFormat_;
        colorDesc.blend_state.enable_blend = true;
        colorDesc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        colorDesc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorDesc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colorDesc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorDesc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorDesc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        // Vertex layout: float2 pos, float4 color
        SDL_GPUVertexBufferDescription vbDesc = {};
        vbDesc.slot = 0;
        vbDesc.pitch = sizeof(ColorVertex);
        vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[2] = {};
        // pos
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        // color
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
        attrs[1].offset = offsetof(ColorVertex, r);

        SDL_GPUVertexInputState vertexInput = {};
        vertexInput.vertex_buffer_descriptions = &vbDesc;
        vertexInput.num_vertex_buffers = 1;
        vertexInput.vertex_attributes = attrs;
        vertexInput.num_vertex_attributes = 2;

        SDL_GPUGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.vertex_shader = vs;
        pipelineInfo.fragment_shader = fs;
        pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipelineInfo.vertex_input_state = vertexInput;
        pipelineInfo.target_info.num_color_targets = 1;
        pipelineInfo.target_info.color_target_descriptions = &colorDesc;

        // Rasterizer: default (fill, no cull)
        pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

        colorPipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipelineInfo);
        if (!colorPipeline_) {
            LOG_ERROR("Failed to create color pipeline: %s", SDL_GetError());
            throw std::runtime_error("Failed to create color pipeline");
        }

        SDL_ReleaseGPUShader(device_, vs);
        SDL_ReleaseGPUShader(device_, fs);
    }

    // --- Texture pipeline ---
    {
        SDL_GPUShader* vs = createShader(SDL_GPU_SHADERSTAGE_VERTEX,
            shaders::texture_vert, shaders::texture_vert_size, 0, 1);
        SDL_GPUShader* fs = createShader(SDL_GPU_SHADERSTAGE_FRAGMENT,
            shaders::texture_frag, shaders::texture_frag_size, 1, 0);

        // Premultiplied alpha blend (Skia output is premultiplied)
        SDL_GPUColorTargetDescription colorDesc = {};
        colorDesc.format = swapchainFormat_;
        colorDesc.blend_state.enable_blend = true;
        colorDesc.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorDesc.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorDesc.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        colorDesc.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        colorDesc.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        colorDesc.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

        // Vertex layout: float2 pos, float2 uv
        SDL_GPUVertexBufferDescription vbDesc = {};
        vbDesc.slot = 0;
        vbDesc.pitch = sizeof(TextureVertex);
        vbDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

        SDL_GPUVertexAttribute attrs[2] = {};
        attrs[0].location = 0;
        attrs[0].buffer_slot = 0;
        attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].buffer_slot = 0;
        attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
        attrs[1].offset = offsetof(TextureVertex, u);

        SDL_GPUVertexInputState vertexInput = {};
        vertexInput.vertex_buffer_descriptions = &vbDesc;
        vertexInput.num_vertex_buffers = 1;
        vertexInput.vertex_attributes = attrs;
        vertexInput.num_vertex_attributes = 2;

        SDL_GPUGraphicsPipelineCreateInfo pipelineInfo = {};
        pipelineInfo.vertex_shader = vs;
        pipelineInfo.fragment_shader = fs;
        pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipelineInfo.vertex_input_state = vertexInput;
        pipelineInfo.target_info.num_color_targets = 1;
        pipelineInfo.target_info.color_target_descriptions = &colorDesc;
        pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;

        texturePipeline_ = SDL_CreateGPUGraphicsPipeline(device_, &pipelineInfo);
        if (!texturePipeline_) {
            LOG_ERROR("Failed to create texture pipeline: %s", SDL_GetError());
            throw std::runtime_error("Failed to create texture pipeline");
        }

        SDL_ReleaseGPUShader(device_, vs);
        SDL_ReleaseGPUShader(device_, fs);
    }

    LOG_INFO("GPU pipelines created (color + texture)");
}

// ---------------------------------------------------------------------------
// Transfer buffer management
// ---------------------------------------------------------------------------

void GPUContext::ensureTransferBuffer(uint32_t needed) {
    if (transferBuf_ && transferBufSize_ >= needed) return;

    if (transferBuf_) {
        SDL_ReleaseGPUTransferBuffer(device_, transferBuf_);
    }

    // Round up to next power of 2 (min 64KB)
    uint32_t size = 65536;
    while (size < needed) size *= 2;

    SDL_GPUTransferBufferCreateInfo info = {};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = size;
    transferBuf_ = SDL_CreateGPUTransferBuffer(device_, &info);
    transferBufSize_ = size;
}

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------

SDL_GPUCommandBuffer* GPUContext::beginFrame() {
    return SDL_AcquireGPUCommandBuffer(device_);
}

bool GPUContext::acquireSwapchain(SDL_GPUCommandBuffer* cmd,
                                  SDL_GPUTexture*& outTexture,
                                  uint32_t& outW, uint32_t& outH) {
    return SDL_WaitAndAcquireGPUSwapchainTexture(cmd, sdlWindow_,
                                                  &outTexture, &outW, &outH);
}

void GPUContext::submit(SDL_GPUCommandBuffer* cmd) {
    SDL_SubmitGPUCommandBuffer(cmd);
}

// ---------------------------------------------------------------------------
// Texture helpers
// ---------------------------------------------------------------------------

SDL_GPUTexture* GPUContext::createTexture2D(uint32_t w, uint32_t h,
                                             SDL_GPUTextureFormat fmt,
                                             SDL_GPUTextureUsageFlags usage) {
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = fmt;
    info.usage = usage;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    return SDL_CreateGPUTexture(device_, &info);
}

void GPUContext::uploadToTexture(SDL_GPUCommandBuffer* cmd,
                                 SDL_GPUTexture* dst,
                                 const void* pixels,
                                 uint32_t w, uint32_t h, uint32_t pitch) {
    uint32_t dataSize = pitch * h;
    ensureTransferBuffer(dataSize);

    void* mapped = SDL_MapGPUTransferBuffer(device_, transferBuf_, true);
    if (!mapped) {
        LOG_ERROR("Failed to map transfer buffer: %s", SDL_GetError());
        return;
    }
    memcpy(mapped, pixels, dataSize);
    SDL_UnmapGPUTransferBuffer(device_, transferBuf_);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = transferBuf_;
    src.offset = 0;
    src.pixels_per_row = w;
    src.rows_per_layer = h;

    SDL_GPUTextureRegion region = {};
    region.texture = dst;
    region.w = w;
    region.h = h;
    region.d = 1;

    SDL_UploadToGPUTexture(copyPass, &src, &region, true);
    SDL_EndGPUCopyPass(copyPass);
}

void GPUContext::releaseTexture(SDL_GPUTexture* tex) {
    if (tex && device_) SDL_ReleaseGPUTexture(device_, tex);
}

// ---------------------------------------------------------------------------
// Buffer helpers
// ---------------------------------------------------------------------------

SDL_GPUBuffer* GPUContext::createVertexBuffer(uint32_t sizeBytes) {
    SDL_GPUBufferCreateInfo info = {};
    info.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    info.size = sizeBytes;
    return SDL_CreateGPUBuffer(device_, &info);
}

void GPUContext::uploadToBuffer(SDL_GPUCommandBuffer* cmd,
                                SDL_GPUBuffer* dst,
                                const void* data, uint32_t sizeBytes) {
    ensureTransferBuffer(sizeBytes);

    void* mapped = SDL_MapGPUTransferBuffer(device_, transferBuf_, true);
    if (!mapped) return;
    memcpy(mapped, data, sizeBytes);
    SDL_UnmapGPUTransferBuffer(device_, transferBuf_);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = transferBuf_;
    src.offset = 0;

    SDL_GPUBufferRegion region = {};
    region.buffer = dst;
    region.offset = 0;
    region.size = sizeBytes;

    SDL_UploadToGPUBuffer(copyPass, &src, &region, true);
    SDL_EndGPUCopyPass(copyPass);
}

void GPUContext::releaseBuffer(SDL_GPUBuffer* buf) {
    if (buf && device_) SDL_ReleaseGPUBuffer(device_, buf);
}

} // namespace bro::render
