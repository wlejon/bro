#include "canvas/canvas_scene.h"
#include "render/skia_backend.h"
#include "render/gpu_context.h"

#include <SDL3/SDL_gpu.h>
#include <cmath>

namespace bro::canvas {

uint64_t CanvasScene::getOrCreateFont(const std::string& fontStr) {
    auto it = fontCache_.find(fontStr);
    if (it != fontCache_.end()) return it->second;

    auto pf = parseCSSFont(fontStr);
    uint64_t handle = renderer_->createFont(pf.family, pf.size, pf.weight, pf.italic);
    fontCache_[fontStr] = handle;
    return handle;
}

void CanvasScene::onCleanup() {
    textCache_.clear();
    for (auto& [k, h] : fontCache_) renderer_->deleteFont(h);
    fontCache_.clear();
    if (vertexBuf_ && gpu_) {
        gpu_->releaseBuffer(vertexBuf_);
        vertexBuf_ = nullptr;
        vertexBufSize_ = 0;
    }
}

// ---------------------------------------------------------------------------
// prepareFrame — build vertex data and upload to GPU (before render pass)
// ---------------------------------------------------------------------------

void CanvasScene::prepareFrame(render::GPUContext* gpu, SDL_GPUCommandBuffer* cmd,
                                int w, int h) {
    if (!gpu || !cmd) return;
    width_ = w;
    height_ = h;

    using CV = render::ColorVertex;
    using TV = render::TextureVertex;

    std::vector<CV> colorVerts;
    std::vector<TV> texVerts;
    textDraws_.clear();

    uint8_t fillR = 0, fillG = 0, fillB = 0, fillA = 255;
    uint8_t strokeR = 0, strokeG = 0, strokeB = 0, strokeA = 255;
    float lineWidth = 1.0f;
    float globalAlpha = 1.0f;
    std::string currentFont = "16px sans-serif";
    uint64_t fontHandle = 0;
    float tx = 0, ty = 0;

    struct SavedState {
        uint8_t fR, fG, fB, fA, sR, sG, sB, sA;
        float lw, ga, tx, ty;
        std::string font;
        uint64_t fontHandle;
    };
    std::vector<SavedState> stack;

    auto pushQuad = [&](float x, float y, float qw, float qh,
                        float r, float g, float b, float a) {
        float x2 = x + qw, y2 = y + qh;
        colorVerts.push_back({x,  y,  r, g, b, a});
        colorVerts.push_back({x2, y,  r, g, b, a});
        colorVerts.push_back({x2, y2, r, g, b, a});
        colorVerts.push_back({x,  y,  r, g, b, a});
        colorVerts.push_back({x2, y2, r, g, b, a});
        colorVerts.push_back({x,  y2, r, g, b, a});
    };

    auto pushLine = [&](float x1, float y1, float x2, float y2,
                        float r, float g, float b, float a, float thickness) {
        float dx = x2 - x1, dy = y2 - y1;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 0.001f) return;
        float nx = -dy / len * thickness * 0.5f;
        float ny =  dx / len * thickness * 0.5f;
        colorVerts.push_back({x1 + nx, y1 + ny, r, g, b, a});
        colorVerts.push_back({x1 - nx, y1 - ny, r, g, b, a});
        colorVerts.push_back({x2 - nx, y2 - ny, r, g, b, a});
        colorVerts.push_back({x1 + nx, y1 + ny, r, g, b, a});
        colorVerts.push_back({x2 - nx, y2 - ny, r, g, b, a});
        colorVerts.push_back({x2 + nx, y2 + ny, r, g, b, a});
    };

    auto* skia = static_cast<render::SkiaRenderer*>(renderer_);

    for (auto& c : canvas_.commands()) {
        switch (c.type) {
        case CmdType::SetFillStyle:
            fillR = c.r; fillG = c.g; fillB = c.b; fillA = c.a;
            break;
        case CmdType::SetStrokeStyle:
            strokeR = c.r; strokeG = c.g; strokeB = c.b; strokeA = c.a;
            break;
        case CmdType::SetLineWidth:
            lineWidth = c.f;
            break;
        case CmdType::SetGlobalAlpha:
            globalAlpha = c.f;
            break;
        case CmdType::SetFont:
            currentFont = c.text;
            fontHandle = getOrCreateFont(currentFont);
            break;

        case CmdType::FillRect: {
            float a = (fillA / 255.0f) * globalAlpha;
            pushQuad(c.x + tx, c.y + ty, c.w, c.h,
                     fillR / 255.0f, fillG / 255.0f, fillB / 255.0f, a);
            break;
        }
        case CmdType::StrokeRect: {
            float a = (strokeA / 255.0f) * globalAlpha;
            float r = strokeR / 255.0f, g = strokeG / 255.0f, b = strokeB / 255.0f;
            float x = c.x + tx, y = c.y + ty;
            float lw = std::max(lineWidth, 1.0f);
            pushLine(x, y, x + c.w, y, r, g, b, a, lw);
            pushLine(x + c.w, y, x + c.w, y + c.h, r, g, b, a, lw);
            pushLine(x + c.w, y + c.h, x, y + c.h, r, g, b, a, lw);
            pushLine(x, y + c.h, x, y, r, g, b, a, lw);
            break;
        }
        case CmdType::ClearRect: {
            pushQuad(c.x + tx, c.y + ty, c.w, c.h,
                     0.0f, 0.0f, 0.0f, 1.0f);
            break;
        }
        case CmdType::FillText: {
            if (!fontHandle) fontHandle = getOrCreateFont(currentFont);
            render::Color col{fillR, fillG, fillB, (uint8_t)(fillA * globalAlpha)};
            int tw = 0, th = 0;
            SDL_GPUTexture* tex = skia->renderTextToTexture(cmd, c.text, fontHandle, col, tw, th);
            if (tex) {
                float dx = c.x + tx;
                float dy = c.y + ty - th * 0.75f;
                uint32_t base = (uint32_t)texVerts.size();
                float fw = (float)tw, fh = (float)th;
                texVerts.push_back({dx,      dy,      0.0f, 0.0f});
                texVerts.push_back({dx + fw, dy,      1.0f, 0.0f});
                texVerts.push_back({dx + fw, dy + fh, 1.0f, 1.0f});
                texVerts.push_back({dx,      dy,      0.0f, 0.0f});
                texVerts.push_back({dx + fw, dy + fh, 1.0f, 1.0f});
                texVerts.push_back({dx,      dy + fh, 0.0f, 1.0f});
                textDraws_.push_back({tex, base, 6});
            }
            break;
        }

        case CmdType::Save:
            stack.push_back({fillR, fillG, fillB, fillA, strokeR, strokeG, strokeB, strokeA,
                            lineWidth, globalAlpha, tx, ty, currentFont, fontHandle});
            break;
        case CmdType::Restore:
            if (!stack.empty()) {
                auto& s = stack.back();
                fillR = s.fR; fillG = s.fG; fillB = s.fB; fillA = s.fA;
                strokeR = s.sR; strokeG = s.sG; strokeB = s.sB; strokeA = s.sA;
                lineWidth = s.lw; globalAlpha = s.ga; tx = s.tx; ty = s.ty;
                currentFont = s.font; fontHandle = s.fontHandle;
                stack.pop_back();
            }
            break;
        case CmdType::Translate:
            tx += c.x; ty += c.y;
            break;
        case CmdType::Rotate:
        case CmdType::Scale:
            break;
        }
    }

    // Store counts for onRender
    colorVertCount_ = (uint32_t)colorVerts.size();
    colorBytes_ = (uint32_t)(colorVerts.size() * sizeof(CV));
    uint32_t texBytes = (uint32_t)(texVerts.size() * sizeof(TV));
    uint32_t totalBytes = colorBytes_ + texBytes;

    if (totalBytes == 0) return;

    // Ensure GPU vertex buffer is large enough
    if (!vertexBuf_ || vertexBufSize_ < totalBytes) {
        if (vertexBuf_) gpu->releaseBuffer(vertexBuf_);
        uint32_t size = 65536;
        while (size < totalBytes) size *= 2;
        vertexBuf_ = gpu->createVertexBuffer(size);
        vertexBufSize_ = size;
    }

    // Combine into one upload: [color verts | texture verts]
    std::vector<uint8_t> combined(totalBytes);
    if (colorBytes_ > 0)
        memcpy(combined.data(), colorVerts.data(), colorBytes_);
    if (texBytes > 0)
        memcpy(combined.data() + colorBytes_, texVerts.data(), texBytes);

    gpu->uploadToBuffer(cmd, vertexBuf_, combined.data(), totalBytes);
}

// ---------------------------------------------------------------------------
// onRender — issue draw calls (inside render pass, vertex data already uploaded)
// ---------------------------------------------------------------------------

void CanvasScene::onRender(render::GPUContext* gpu, SDL_GPUCommandBuffer* cmd,
                            SDL_GPURenderPass* pass,
                            int w, int h, double) {
    if (!gpu || !cmd || !pass || !vertexBuf_) return;
    if (colorVertCount_ == 0 && textDraws_.empty()) return;

    float viewport[2] = {(float)w, (float)h};

    // Draw colored geometry
    if (colorVertCount_ > 0) {
        SDL_BindGPUGraphicsPipeline(pass, gpu->colorPipeline());
        SDL_PushGPUVertexUniformData(cmd, 0, viewport, sizeof(viewport));

        SDL_GPUBufferBinding binding = {};
        binding.buffer = vertexBuf_;
        binding.offset = 0;
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);
        SDL_DrawGPUPrimitives(pass, colorVertCount_, 1, 0, 0);
    }

    // Draw textured quads (text)
    if (!textDraws_.empty()) {
        SDL_BindGPUGraphicsPipeline(pass, gpu->texturePipeline());
        SDL_PushGPUVertexUniformData(cmd, 0, viewport, sizeof(viewport));

        SDL_GPUBufferBinding binding = {};
        binding.buffer = vertexBuf_;
        binding.offset = colorBytes_;
        SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

        for (auto& td : textDraws_) {
            SDL_GPUTextureSamplerBinding texBind = {};
            texBind.texture = td.tex;
            texBind.sampler = gpu->linearSampler();
            SDL_BindGPUFragmentSamplers(pass, 0, &texBind, 1);
            SDL_DrawGPUPrimitives(pass, td.vertexCount, 1, td.firstVertex, 0);
        }
    }
}

} // namespace bro::canvas
