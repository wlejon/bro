#pragma once

#include <SDL3/SDL_gpu.h>
#include <cstdint>
#include <cstddef>

namespace bro::platform { class Window; }

namespace bro::render {

struct ColorVertex {
    float x, y;
    float r, g, b, a;
};

struct TextureVertex {
    float x, y;
    float u, v;
};

class GPUContext {
public:
    explicit GPUContext(platform::Window& window, bool debug = false);
    ~GPUContext();

    GPUContext(const GPUContext&) = delete;
    GPUContext& operator=(const GPUContext&) = delete;

    SDL_GPUDevice* device() const { return device_; }

    // --- Frame lifecycle ---
    SDL_GPUCommandBuffer* beginFrame();
    bool acquireSwapchain(SDL_GPUCommandBuffer* cmd,
                          SDL_GPUTexture*& outTexture,
                          uint32_t& outW, uint32_t& outH);
    void submit(SDL_GPUCommandBuffer* cmd);

    // --- Texture helpers ---
    SDL_GPUTexture* createTexture2D(uint32_t w, uint32_t h,
                                     SDL_GPUTextureFormat fmt,
                                     SDL_GPUTextureUsageFlags usage);
    void uploadToTexture(SDL_GPUCommandBuffer* cmd,
                         SDL_GPUTexture* dst,
                         const void* pixels,
                         uint32_t w, uint32_t h, uint32_t pitch);
    void releaseTexture(SDL_GPUTexture* tex);

    // --- Buffer helpers ---
    SDL_GPUBuffer* createVertexBuffer(uint32_t sizeBytes);
    void uploadToBuffer(SDL_GPUCommandBuffer* cmd,
                        SDL_GPUBuffer* dst,
                        const void* data, uint32_t sizeBytes);
    void releaseBuffer(SDL_GPUBuffer* buf);

    // --- Pipeline access ---
    SDL_GPUGraphicsPipeline* colorPipeline() const { return colorPipeline_; }
    SDL_GPUGraphicsPipeline* texturePipeline() const { return texturePipeline_; }
    SDL_GPUSampler* linearSampler() const { return linearSampler_; }

    SDL_GPUTextureFormat swapchainFormat() const { return swapchainFormat_; }

private:
    void createPipelines();
    SDL_GPUShader* createShader(SDL_GPUShaderStage stage,
                                const uint8_t* code, size_t codeSize,
                                uint32_t numSamplers, uint32_t numUniformBuffers);

    SDL_GPUDevice* device_ = nullptr;
    SDL_Window* sdlWindow_ = nullptr;
    SDL_GPUGraphicsPipeline* colorPipeline_ = nullptr;
    SDL_GPUGraphicsPipeline* texturePipeline_ = nullptr;
    SDL_GPUSampler* linearSampler_ = nullptr;
    SDL_GPUTextureFormat swapchainFormat_ = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;

    // Reusable transfer buffer (grown on demand)
    SDL_GPUTransferBuffer* transferBuf_ = nullptr;
    uint32_t transferBufSize_ = 0;
    void ensureTransferBuffer(uint32_t needed);
};

} // namespace bro::render
