#include "render/command_replayer.h"

#include <variant>

namespace bro::render {

CommandReplayer::CommandReplayer(Renderer* dst) : dst_(dst) {}

uint64_t CommandReplayer::resolveFont(std::string_view family, float size,
                                      int weight, bool italic) {
    for (const auto& fe : fontCache_) {
        if (fe.size == size && fe.weight == weight && fe.italic == italic
            && fe.family == family) {
            return fe.handle;
        }
    }
    uint64_t handle = dst_->createFont(family, size, weight, italic);
    fontCache_.push_back(FontEntry{std::string(family), size, weight, italic, handle});
    return handle;
}

void CommandReplayer::replay(const CommandBuffer& buffer) {
    for (const auto& cmd : buffer.commands()) {
        std::visit([&](const auto& c) {
            using T = std::decay_t<decltype(c)>;

            if constexpr (std::is_same_v<T, Cmd_Clear>) {
                dst_->clear(c.color);
            } else if constexpr (std::is_same_v<T, Cmd_FillRect>) {
                dst_->fillRect(c.x, c.y, c.w, c.h, c.color);
            } else if constexpr (std::is_same_v<T, Cmd_DrawRect>) {
                dst_->drawRect(c.x, c.y, c.w, c.h, c.color);
            } else if constexpr (std::is_same_v<T, Cmd_FillRoundRect>) {
                dst_->fillRoundRect(c.x, c.y, c.w, c.h, c.rx, c.ry, c.color);
            } else if constexpr (std::is_same_v<T, Cmd_DrawRoundRect>) {
                dst_->drawRoundRect(c.x, c.y, c.w, c.h, c.rx, c.ry, c.color);
            } else if constexpr (std::is_same_v<T, Cmd_FillRoundRectRadii>) {
                dst_->fillRoundRectRadii(c.x, c.y, c.w, c.h, c.r, c.color);
            } else if constexpr (std::is_same_v<T, Cmd_DrawRoundRectRadii>) {
                dst_->drawRoundRectRadii(c.x, c.y, c.w, c.h, c.r, c.strokeWidth, c.color);
            } else if constexpr (std::is_same_v<T, Cmd_DrawBoxShadow>) {
                dst_->drawBoxShadow(c.x, c.y, c.w, c.h, c.rx, c.ry,
                                    c.offsetX, c.offsetY, c.blur, c.spread,
                                    c.color, c.inset);
            } else if constexpr (std::is_same_v<T, Cmd_DrawBoxShadowRadii>) {
                dst_->drawBoxShadowRadii(c.x, c.y, c.w, c.h, c.r,
                                         c.offsetX, c.offsetY, c.blur, c.spread,
                                         c.color, c.inset);

            } else if constexpr (std::is_same_v<T, Cmd_DrawText>) {
                std::string_view text = buffer.stringAt(c.textOffset, c.textLen);
                std::string_view family = buffer.stringAt(c.familyOffset, c.familyLen);
                uint64_t handle = resolveFont(family, c.fontSize, c.fontWeight, c.fontItalic);
                if (c.letterSpacing != 0.0f || c.blur != 0.0f) {
                    dst_->drawTextEx(text, c.x, c.y, handle, c.color,
                                     c.letterSpacing, c.blur);
                } else {
                    dst_->drawText(text, c.x, c.y, handle, c.color);
                }

            } else if constexpr (std::is_same_v<T, Cmd_DrawLine>) {
                dst_->drawLine(c.x1, c.y1, c.x2, c.y2, c.color, c.thickness);
            } else if constexpr (std::is_same_v<T, Cmd_DrawImage>) {
                dst_->drawImage(buffer.bytesAt(c.dataOffset), c.dataLen,
                                c.x, c.y, c.w, c.h);
            } else if constexpr (std::is_same_v<T, Cmd_DrawPixelsRGBA>) {
                dst_->drawPixelsRGBA(reinterpret_cast<const uint8_t*>(
                                         buffer.bytesAt(c.pixelsOffset)),
                                     c.srcW, c.srcH, c.stride,
                                     c.x, c.y, c.w, c.h);

            } else if constexpr (std::is_same_v<T, Cmd_DrawCircle>) {
                dst_->drawCircle(c.cx, c.cy, c.r, c.fill, c.stroke, c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawEllipse>) {
                dst_->drawEllipse(c.cx, c.cy, c.rx, c.ry, c.fill, c.stroke, c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawPath>) {
                dst_->drawPath(buffer.stringAt(c.pathOffset, c.pathLen),
                               c.fill, c.stroke, c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawPolygon>) {
                dst_->drawPolygon(buffer.spanAt<PointF>(c.pointsOffset, c.pointsLen),
                                  c.fill, c.stroke, c.strokeWidth);
            } else if constexpr (std::is_same_v<T, Cmd_DrawPolyline>) {
                dst_->drawPolyline(buffer.spanAt<PointF>(c.pointsOffset, c.pointsLen),
                                   c.stroke, c.strokeWidth);

            } else if constexpr (std::is_same_v<T, Cmd_Save>) {
                dst_->save();
            } else if constexpr (std::is_same_v<T, Cmd_Restore>) {
                dst_->restore();
            } else if constexpr (std::is_same_v<T, Cmd_SaveLayerAlpha>) {
                dst_->saveLayerAlpha(c.alpha);
            } else if constexpr (std::is_same_v<T, Cmd_SaveLayerWithFilter>) {
                dst_->saveLayerWithFilter(
                    buffer.spanAt<CssFilterParams>(c.filtersOffset, c.filtersLen),
                    c.x, c.y, c.w, c.h);
            } else if constexpr (std::is_same_v<T, Cmd_Translate>) {
                dst_->translate(c.dx, c.dy);
            } else if constexpr (std::is_same_v<T, Cmd_Scale>) {
                dst_->scale(c.sx, c.sy);
            } else if constexpr (std::is_same_v<T, Cmd_Rotate>) {
                dst_->rotate(c.degrees);
            } else if constexpr (std::is_same_v<T, Cmd_Concat>) {
                dst_->concat(c.a, c.b, c.c, c.d, c.e, c.f);
            } else if constexpr (std::is_same_v<T, Cmd_Concat4x4>) {
                dst_->concat4x4(c.m);
            } else if constexpr (std::is_same_v<T, Cmd_SetClip>) {
                dst_->setClip(c.x, c.y, c.w, c.h);
            } else if constexpr (std::is_same_v<T, Cmd_SetClipRRect>) {
                dst_->setClipRRect(c.x, c.y, c.w, c.h, c.r);
            } else if constexpr (std::is_same_v<T, Cmd_ResetClip>) {
                dst_->resetClip();
            } else if constexpr (std::is_same_v<T, Cmd_SetClipPolygon>) {
                dst_->setClipPolygon(buffer.spanAt<PointF>(c.pointsOffset, c.pointsLen));

            } else if constexpr (std::is_same_v<T, Cmd_FillLinearGradient>) {
                dst_->fillLinearGradient(c.x, c.y, c.w, c.h,
                                         c.startX, c.startY, c.endX, c.endY,
                                         buffer.spanAt<ColorStop>(c.stopsOffset, c.stopsLen));
            } else if constexpr (std::is_same_v<T, Cmd_FillRadialGradient>) {
                dst_->fillRadialGradient(c.x, c.y, c.w, c.h,
                                         c.cx, c.cy, c.rx, c.ry,
                                         buffer.spanAt<ColorStop>(c.stopsOffset, c.stopsLen));
            } else if constexpr (std::is_same_v<T, Cmd_FillConicGradient>) {
                dst_->fillConicGradient(c.x, c.y, c.w, c.h,
                                        c.cx, c.cy, c.angleDeg,
                                        buffer.spanAt<ColorStop>(c.stopsOffset, c.stopsLen));

            } else if constexpr (std::is_same_v<T, Cmd_BeginFrame>) {
                dst_->beginFrame(c.width, c.height);
            } else if constexpr (std::is_same_v<T, Cmd_EndFrame>) {
                dst_->endFrame();

            } else if constexpr (std::is_same_v<T, Cmd_LayerBreak>) {
                if (onLayerBreak_) {
                    onLayerBreak_(c.kind, c.canvasScene, c.directTexture,
                                  c.x, c.y, c.w, c.h);
                }
            } else if constexpr (std::is_same_v<T, Cmd_BlitCanvasInline>) {
                if (onBlitCanvasInline_) {
                    onBlitCanvasInline_(c.canvasScene, c.x, c.y, c.w, c.h);
                }
            }
        }, cmd);
    }
}

} // namespace bro::render
