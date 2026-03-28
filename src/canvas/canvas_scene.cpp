#include "canvas/canvas_scene.h"
#include "render/skia_backend.h"

namespace bro::canvas {

uint64_t CanvasScene::getOrCreateFont(const std::string& fontStr) {
    auto it = fontCache_.find(fontStr);
    if (it != fontCache_.end()) return it->second;

    auto pf = parseCSSFont(fontStr);
    uint64_t handle = renderer_->createFont(pf.family, pf.size, pf.weight, pf.italic);
    fontCache_[fontStr] = handle;
    return handle;
}

void CanvasScene::onRender(SDL_Renderer* sdl, int w, int h, double) {
    width_ = w;
    height_ = h;

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

    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);

    for (auto& cmd : canvas_.commands()) {
        switch (cmd.type) {
        case CmdType::SetFillStyle:
            fillR = cmd.r; fillG = cmd.g; fillB = cmd.b; fillA = cmd.a;
            break;
        case CmdType::SetStrokeStyle:
            strokeR = cmd.r; strokeG = cmd.g; strokeB = cmd.b; strokeA = cmd.a;
            break;
        case CmdType::SetLineWidth:
            lineWidth = cmd.f;
            break;
        case CmdType::SetGlobalAlpha:
            globalAlpha = cmd.f;
            break;
        case CmdType::SetFont:
            currentFont = cmd.text;
            fontHandle = getOrCreateFont(currentFont);
            break;

        case CmdType::FillRect: {
            uint8_t a = (uint8_t)(fillA * globalAlpha);
            SDL_SetRenderDrawColor(sdl, fillR, fillG, fillB, a);
            SDL_FRect r = {cmd.x + tx, cmd.y + ty, cmd.w, cmd.h};
            SDL_RenderFillRect(sdl, &r);
            break;
        }
        case CmdType::StrokeRect: {
            uint8_t a = (uint8_t)(strokeA * globalAlpha);
            SDL_SetRenderDrawColor(sdl, strokeR, strokeG, strokeB, a);
            float x = cmd.x + tx, y = cmd.y + ty;
            SDL_RenderLine(sdl, x, y, x + cmd.w, y);
            SDL_RenderLine(sdl, x + cmd.w, y, x + cmd.w, y + cmd.h);
            SDL_RenderLine(sdl, x + cmd.w, y + cmd.h, x, y + cmd.h);
            SDL_RenderLine(sdl, x, y + cmd.h, x, y);
            break;
        }
        case CmdType::ClearRect: {
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(sdl, 0, 0, 0, 255);
            SDL_FRect r = {cmd.x + tx, cmd.y + ty, cmd.w, cmd.h};
            SDL_RenderFillRect(sdl, &r);
            SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
            break;
        }
        case CmdType::FillText: {
            if (!fontHandle) fontHandle = getOrCreateFont(currentFont);
            auto* skia = static_cast<render::SkiaRenderer*>(renderer_);
            render::Color col{fillR, fillG, fillB, (uint8_t)(fillA * globalAlpha)};
            int tw = 0, th = 0;
            SDL_Texture* tex = skia->renderTextToTexture(cmd.text, fontHandle, col, tw, th);
            if (tex) {
                SDL_FRect dst = {cmd.x + tx, cmd.y + ty - th * 0.75f, (float)tw, (float)th};
                SDL_RenderTexture(sdl, tex, nullptr, &dst);
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
            tx += cmd.x; ty += cmd.y;
            break;
        case CmdType::Rotate:
        case CmdType::Scale:
            break;
        }
    }
}

} // namespace bro::canvas
