#include "canvas/canvas_scene.h"
#include "render/skia_backend.h"
#include "render/gl_context.h"

#include <cmath>
#include <cstring>

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
    if (vertexBuf_) {
        glDeleteBuffers(1, &vertexBuf_);
        vertexBuf_ = 0;
        vertexBufSize_ = 0;
    }
    if (colorVAO_) { glDeleteVertexArrays(1, &colorVAO_); colorVAO_ = 0; }
    if (textureVAO_) { glDeleteVertexArrays(1, &textureVAO_); textureVAO_ = 0; }
}

// ---------------------------------------------------------------------------
// prepareFrame — build vertex data and upload to GPU
// ---------------------------------------------------------------------------

void CanvasScene::prepareFrame(render::GLContext* gl, int w, int h) {
    if (!gl) return;
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
            GLuint tex = skia->renderTextToTexture(c.text, fontHandle, col, tw, th);
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

    // Ensure GL vertex buffer is large enough
    if (!vertexBuf_ || vertexBufSize_ < totalBytes) {
        if (vertexBuf_) glDeleteBuffers(1, &vertexBuf_);
        uint32_t size = 65536;
        while (size < totalBytes) size *= 2;
        glGenBuffers(1, &vertexBuf_);
        glBindBuffer(GL_ARRAY_BUFFER, vertexBuf_);
        glBufferData(GL_ARRAY_BUFFER, size, nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        vertexBufSize_ = size;
    }

    // Combine into one upload: [color verts | texture verts]
    std::vector<uint8_t> combined(totalBytes);
    if (colorBytes_ > 0)
        memcpy(combined.data(), colorVerts.data(), colorBytes_);
    if (texBytes > 0)
        memcpy(combined.data() + colorBytes_, texVerts.data(), texBytes);

    glBindBuffer(GL_ARRAY_BUFFER, vertexBuf_);
    glBufferSubData(GL_ARRAY_BUFFER, 0, totalBytes, combined.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Create VAOs if needed (re-bind buffer each time since buffer may be reallocated)
    if (!colorVAO_) glGenVertexArrays(1, &colorVAO_);
    if (!textureVAO_) glGenVertexArrays(1, &textureVAO_);

    // Setup color VAO
    glBindVertexArray(colorVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuf_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(CV), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(CV), (void*)offsetof(CV, r));
    glBindVertexArray(0);

    // Setup texture VAO (vertices start at colorBytes_ offset)
    glBindVertexArray(textureVAO_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuf_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(TV), (void*)(uintptr_t)colorBytes_);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(TV), (void*)(uintptr_t)(colorBytes_ + offsetof(TV, u)));
    glBindVertexArray(0);
}

// ---------------------------------------------------------------------------
// onRender — issue draw calls
// ---------------------------------------------------------------------------

void CanvasScene::onRender(render::GLContext* gl, int w, int h, double) {
    if (!gl || !vertexBuf_) return;
    if (colorVertCount_ == 0 && textDraws_.empty()) return;

    float viewport[2] = {(float)w, (float)h};

    // Draw colored geometry
    if (colorVertCount_ > 0) {
        glUseProgram(gl->colorProgram());
        glUniform2fv(gl->colorViewportLoc(), 1, viewport);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        glBindVertexArray(colorVAO_);
        glDrawArrays(GL_TRIANGLES, 0, colorVertCount_);
        glBindVertexArray(0);
    }

    // Draw textured quads (text)
    if (!textDraws_.empty()) {
        glUseProgram(gl->textureProgram());
        glUniform2fv(gl->textureViewportLoc(), 1, viewport);
        glUniform1i(gl->textureSamplerLoc(), 0);

        // Premultiplied alpha blend for text
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glBindVertexArray(textureVAO_);

        for (auto& td : textDraws_) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, td.tex);
            glDrawArrays(GL_TRIANGLES, td.firstVertex, td.vertexCount);
        }

        glBindVertexArray(0);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

} // namespace bro::canvas
