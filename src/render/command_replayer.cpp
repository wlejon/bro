#include "render/command_replayer.h"

#include <algorithm>
#include <variant>

namespace bro::render {

CommandReplayer::CommandReplayer(Renderer* dst) : dst_(dst) {}

void CommandReplayer::replayOpenStackOnto(Renderer* dst) {
    if (!dst) return;
    for (auto& frame : openStack_) {
        switch (frame.saveKind) {
            case OpenFrame::Plain:       dst->save(); break;
            case OpenFrame::LayerAlpha:  dst->saveLayerAlpha(frame.alpha); break;
            case OpenFrame::LayerFilter:
                dst->saveLayerWithFilter(frame.filters, frame.flx, frame.fly,
                                         frame.flw, frame.flh);
                break;
            case OpenFrame::LayerBlend:  dst->saveLayerWithBlend(frame.blendMode); break;
        }
        for (auto& op : frame.ops) {
            switch (op.kind) {
                case StackOp::Concat:      dst->concat(op.a, op.b, op.c, op.d, op.e, op.f); break;
                case StackOp::Concat4x4:   dst->concat4x4(op.m); break;
                case StackOp::Translate:   dst->translate(op.x, op.y); break;
                case StackOp::Scale:       dst->scale(op.x, op.y); break;
                case StackOp::Rotate:      dst->rotate(op.degrees); break;
                case StackOp::SetClip:     dst->setClip(op.x, op.y, op.w, op.h); break;
                case StackOp::SetClipRRect:dst->setClipRRect(op.x, op.y, op.w, op.h, op.radii); break;
                case StackOp::SetClipPolygon: dst->setClipPolygon(op.points); break;
                case StackOp::ResetClip:   dst->resetClip(); break;
            }
        }
    }
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
                // Recorded text is normally already shaped — drawing the blob
                // keeps the raster thread out of the shaper entirely.
                if (c.blobIndex != CommandBuffer::kNoTextBlob &&
                    dst_->drawTextBlob(buffer.textBlobAt(c.blobIndex),
                                       c.x, c.y, c.color, c.blur)) {
                    return;
                }
                std::string_view text = buffer.stringAt(c.textOffset, c.textLen);
                std::string_view family = buffer.stringAt(c.familyOffset, c.familyLen);
                FontRef font{family, c.fontSize, c.fontWeight, c.fontItalic};
                if (c.letterSpacing != 0.0f || c.blur != 0.0f ||
                    c.wordSpacing != 0.0f) {
                    dst_->drawTextEx(text, c.x, c.y, font, c.color,
                                     c.letterSpacing, c.blur, c.wordSpacing);
                } else {
                    dst_->drawText(text, c.x, c.y, font, c.color);
                }

            } else if constexpr (std::is_same_v<T, Cmd_DrawLine>) {
                dst_->drawLine(c.x1, c.y1, c.x2, c.y2, c.color, c.thickness);
            } else if constexpr (std::is_same_v<T, Cmd_DrawImage>) {
                dst_->drawImage(buffer.bytesAt(c.dataOffset), c.dataLen,
                                c.x, c.y, c.w, c.h, c.imageId);
            } else if constexpr (std::is_same_v<T, Cmd_DrawSvgMarkup>) {
                dst_->drawSvgMarkup(reinterpret_cast<const char*>(
                                        buffer.bytesAt(c.dataOffset)),
                                    c.dataLen, c.x, c.y, c.w, c.h);
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
            } else if constexpr (std::is_same_v<T, Cmd_DrawSvgPath>) {
                dst_->drawSvgPath(buffer.stringAt(c.pathOffset, c.pathLen), c.rule,
                                  c.fill, buffer.spanAt<ColorStop>(c.fillStopsOffset, c.fillStopsLen),
                                  c.stroke, buffer.spanAt<ColorStop>(c.strokeStopsOffset, c.strokeStopsLen),
                                  c.strokeStyle, buffer.spanAt<float>(c.dashArrOffset, c.dashArrLen));
            } else if constexpr (std::is_same_v<T, Cmd_ClipSvgPath>) {
                dst_->clipSvgPath(buffer.stringAt(c.pathOffset, c.pathLen), c.rule);

            } else if constexpr (std::is_same_v<T, Cmd_Save>) {
                dst_->save();
                openStack_.push_back({});
            } else if constexpr (std::is_same_v<T, Cmd_Restore>) {
                dst_->restore();
                if (!openStack_.empty()) openStack_.pop_back();
            } else if constexpr (std::is_same_v<T, Cmd_SaveLayerAlpha>) {
                dst_->saveLayerAlpha(c.alpha);
                OpenFrame f; f.saveKind = OpenFrame::LayerAlpha; f.alpha = c.alpha;
                openStack_.push_back(std::move(f));
            } else if constexpr (std::is_same_v<T, Cmd_SaveLayerWithFilter>) {
                auto filters = buffer.spanAt<CssFilterParams>(c.filtersOffset, c.filtersLen);
                dst_->saveLayerWithFilter(filters, c.x, c.y, c.w, c.h);
                OpenFrame f; f.saveKind = OpenFrame::LayerFilter;
                f.filters.assign(filters.begin(), filters.end());
                f.flx = c.x; f.fly = c.y; f.flw = c.w; f.flh = c.h;
                openStack_.push_back(std::move(f));
            } else if constexpr (std::is_same_v<T, Cmd_SaveLayerWithBlend>) {
                dst_->saveLayerWithBlend(c.mode);
                OpenFrame f; f.saveKind = OpenFrame::LayerBlend; f.blendMode = c.mode;
                openStack_.push_back(std::move(f));
            } else if constexpr (std::is_same_v<T, Cmd_Translate>) {
                dst_->translate(c.dx, c.dy);
                if (!openStack_.empty()) {
                    StackOp op; op.kind = StackOp::Translate; op.x = c.dx; op.y = c.dy;
                    openStack_.back().ops.push_back(std::move(op));
                }
            } else if constexpr (std::is_same_v<T, Cmd_Scale>) {
                dst_->scale(c.sx, c.sy);
                if (!openStack_.empty()) {
                    StackOp op; op.kind = StackOp::Scale; op.x = c.sx; op.y = c.sy;
                    openStack_.back().ops.push_back(std::move(op));
                }
            } else if constexpr (std::is_same_v<T, Cmd_Rotate>) {
                dst_->rotate(c.degrees);
                if (!openStack_.empty()) {
                    StackOp op; op.kind = StackOp::Rotate; op.degrees = c.degrees;
                    openStack_.back().ops.push_back(std::move(op));
                }
            } else if constexpr (std::is_same_v<T, Cmd_Concat>) {
                dst_->concat(c.a, c.b, c.c, c.d, c.e, c.f);
                if (!openStack_.empty()) {
                    StackOp op; op.kind = StackOp::Concat;
                    op.a = c.a; op.b = c.b; op.c = c.c; op.d = c.d; op.e = c.e; op.f = c.f;
                    openStack_.back().ops.push_back(std::move(op));
                }
            } else if constexpr (std::is_same_v<T, Cmd_Concat4x4>) {
                dst_->concat4x4(c.m);
                if (!openStack_.empty()) {
                    StackOp op; op.kind = StackOp::Concat4x4;
                    std::copy(std::begin(c.m), std::end(c.m), op.m);
                    openStack_.back().ops.push_back(std::move(op));
                }
            } else if constexpr (std::is_same_v<T, Cmd_SetClip>) {
                dst_->setClip(c.x, c.y, c.w, c.h);
                if (!openStack_.empty()) {
                    StackOp op; op.kind = StackOp::SetClip;
                    op.x = c.x; op.y = c.y; op.w = c.w; op.h = c.h;
                    openStack_.back().ops.push_back(std::move(op));
                }
            } else if constexpr (std::is_same_v<T, Cmd_SetClipRRect>) {
                dst_->setClipRRect(c.x, c.y, c.w, c.h, c.r);
                if (!openStack_.empty()) {
                    StackOp op; op.kind = StackOp::SetClipRRect;
                    op.x = c.x; op.y = c.y; op.w = c.w; op.h = c.h; op.radii = c.r;
                    openStack_.back().ops.push_back(std::move(op));
                }
            } else if constexpr (std::is_same_v<T, Cmd_ResetClip>) {
                dst_->resetClip();
                if (!openStack_.empty()) {
                    StackOp op; op.kind = StackOp::ResetClip;
                    openStack_.back().ops.push_back(std::move(op));
                }
            } else if constexpr (std::is_same_v<T, Cmd_SetClipPolygon>) {
                auto pts = buffer.spanAt<PointF>(c.pointsOffset, c.pointsLen);
                dst_->setClipPolygon(pts);
                if (!openStack_.empty()) {
                    StackOp op; op.kind = StackOp::SetClipPolygon;
                    op.points.assign(pts.begin(), pts.end());
                    openStack_.back().ops.push_back(std::move(op));
                }

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
                    onLayerBreak_(c.kind, c.canvasSceneId, c.directTexture,
                                  c.x, c.y, c.w, c.h,
                                  c.clipX, c.clipY, c.clipW, c.clipH);
                }
                // The handler just switched dst_ to a brand-new surface
                // (identity matrix, no clip). Any Save/transform/clip from
                // before the break that hasn't been Restore'd yet — e.g. an
                // ancestor CSS transform wrapping a stacking context that
                // contains this canvas — needs to be re-established, or HTML
                // content painted after the break renders at its untransformed
                // layout position instead of following the ancestor transform.
                replayOpenStackOnto(dst_);
            } else if constexpr (std::is_same_v<T, Cmd_BlitCanvasInline>) {
                if (onBlitCanvasInline_) {
                    onBlitCanvasInline_(c.canvasScene, c.x, c.y, c.w, c.h);
                }
            }
        }, cmd);
    }
}

} // namespace bro::render
