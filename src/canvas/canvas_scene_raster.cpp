// CanvasScene's raster side: command replay onto the SkSurface, and the
// per-frame rasterize the compositor calls. The recording API that fills the
// command buffer lives in canvas_scene.cpp and canvas_scene_state.cpp.

#include "canvas/canvas_scene.h"
#include "util/log.h"

#include <include/core/SkData.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkM44.h>
#include <include/core/SkPixmap.h>

#include <iterator>

namespace bro::canvas {

void CanvasScene::stageCommandsForRaster() {
    if (commands_.empty()) return;
    if (stagedCommands_.empty()) {
        std::swap(commands_, stagedCommands_);
    } else {
        stagedCommands_.insert(stagedCommands_.end(),
            std::make_move_iterator(commands_.begin()),
            std::make_move_iterator(commands_.end()));
        commands_.clear();
    }
}

// Streaming-buffer canvas fast path. When a frame's command buffer consists
// only of putImageData calls, the SkCanvas replay (paint setup, transform
// save/restore, draw-image submission) is pure overhead — the final state of
// the surface is just the bytes from the *last* putImageData. We can skip
// directly to a bulk pixel copy via SkSurface::writePixels, with no draw
// pipeline activity. This is the dominant pattern for streaming visualizations
// (noise fields, audio waveforms, spectrograms, voxel mini-maps).
//
// Conservative check: every command must be kPutImageData. If any other op
// is present (paths, text, transforms, clears, etc.) we fall back to the
// regular replay. The last putImageData must also fit within the surface;
// partial-region writes are handled correctly by writePixels' offset args.
//
// Returns true if the fast path consumed the commands; the caller should
// then clear the buffer and return without running the normal replay.
static bool tryStreamingPutImageDataFastPath(SkSurface* surface,
                                             std::vector<CanvasCmd>& cmds)
{
    if (!surface || cmds.empty()) return false;
    int lastPut = -1;
    for (size_t i = 0; i < cmds.size(); ++i) {
        if (cmds[i].type != CanvasCmd::kPutImageData) return false;
        if (!cmds[i].src.isEmpty()) return false;
        lastPut = static_cast<int>(i);
    }
    if (lastPut < 0) return false;

    const CanvasCmd& cmd = cmds[lastPut];
    if (!cmd.img) return false;
    SkPixmap pm;
    if (!cmd.img->peekPixels(&pm)) return false;

    const int dx = static_cast<int>(cmd.p[0]);
    const int dy = static_cast<int>(cmd.p[1]);
    const int sw = surface->width();
    const int sh = surface->height();
    // writePixels will clip silently to the surface, but reject obviously bad
    // offsets so we do not mask an engine bug with an empty write.
    if (dx + pm.width() <= 0 || dy + pm.height() <= 0 || dx >= sw || dy >= sh) {
        return false;
    }
    surface->writePixels(pm, dx, dy);
    return true;
}

void CanvasScene::replayOne(SkCanvas* c, CanvasCmd& cmd) {
    switch (cmd.type) {
    case CanvasCmd::kFillRect:
    case CanvasCmd::kStrokeRect:
        c->drawRect(SkRect::MakeXYWH(cmd.p[0], cmd.p[1], cmd.p[2], cmd.p[3]), cmd.paint);
        break;
    case CanvasCmd::kClearRect: {
        SkPaint clr;
        clr.setBlendMode(SkBlendMode::kClear);
        c->drawRect(SkRect::MakeXYWH(cmd.p[0], cmd.p[1], cmd.p[2], cmd.p[3]), clr);
        break;
    }
    case CanvasCmd::kStrokePath:
    case CanvasCmd::kFillPath:
        c->drawPath(cmd.path, cmd.paint);
        break;
    case CanvasCmd::kClipPath:
        c->clipPath(cmd.path, true);
        break;
    case CanvasCmd::kFillText:
    case CanvasCmd::kStrokeText: {
        bool scaled = cmd.p[2] > 0.0f && cmd.p[2] < 1.0f;
        if (scaled) {
            c->save();
            c->translate(cmd.p[3], 0.0f);
            c->scale(cmd.p[2], 1.0f);
        }
        // Shaped at record time; this thread only replays glyphs.
        if (cmd.blob) c->drawTextBlob(cmd.blob, cmd.p[0], cmd.p[1], cmd.paint);
        else c->drawSimpleText(cmd.text.data(), cmd.text.size(), SkTextEncoding::kUTF8,
                               cmd.p[0], cmd.p[1], cmd.font, cmd.paint);
        if (scaled) {
            c->restore();
        }
        break;
    }
    case CanvasCmd::kDrawImage:
        c->drawImageRect(cmd.img, cmd.src, cmd.dst, cmd.samp, &cmd.paint,
                         SkCanvas::kStrict_SrcRectConstraint);
        break;
    case CanvasCmd::kPutImageData:
        c->save();
        c->resetMatrix();
        if (!cmd.src.isEmpty()) {
            c->drawImageRect(cmd.img, cmd.src, cmd.dst, cmd.samp, &cmd.paint,
                             SkCanvas::kStrict_SrcRectConstraint);
        } else {
            c->drawImage(cmd.img, cmd.p[0], cmd.p[1], cmd.samp, &cmd.paint);
        }
        c->restore();
        break;
    case CanvasCmd::kSave:    c->save(); break;
    case CanvasCmd::kRestore:
        // Never below the drawing state's own level (see ensureSurface). A
        // surface recreated by a layout resize starts without the saves that
        // were open on the old one, so a later restore can find none.
        if (c->getSaveCount() > 2) c->restore();
        break;
    case CanvasCmd::kTranslate: c->translate(cmd.p[0], cmd.p[1]); break;
    case CanvasCmd::kRotate:    c->rotate(cmd.p[0]); break;
    case CanvasCmd::kScale:     c->scale(cmd.p[0], cmd.p[1]); break;
    case CanvasCmd::kSetTransform: {
        c->resetMatrix();
        SkMatrix m;
        m.setAll(cmd.p[0], cmd.p[2], cmd.p[4], cmd.p[1], cmd.p[3], cmd.p[5], 0, 0, 1);
        c->concat(m);
        break;
    }
    case CanvasCmd::kResetTransform:
        c->resetMatrix();
        break;
    case CanvasCmd::kConcatTransform: {
        SkMatrix m;
        m.setAll(cmd.p[0], cmd.p[2], cmd.p[4], cmd.p[1], cmd.p[3], cmd.p[5], 0, 0, 1);
        c->concat(m);
        break;
    }
    case CanvasCmd::kReset:
        // The recorded save()s and clip()s die with the state stack, and the
        // transform goes back to the identity: restoring to the SkCanvas's
        // base level drops every one of them, including a clip made outside
        // any save(), which lives on the level ensureSurface opened above the
        // base — reopened here for the next one.
        c->restoreToCount(1);
        c->save();
        c->clear(SK_ColorTRANSPARENT);
        break;
    }
}

void CanvasScene::replayThroughFilter(SkCanvas* c, CanvasCmd& cmd, const SkM44& ctm,
                                      const sk_sp<SkImageFilter>& filter) {
    // The layer is opened under the identity matrix so the filter's lengths
    // (a blur radius, a shadow or drop-shadow offset) are canvas pixels and not
    // scaled by the current transform; the draw itself then runs under the
    // transform it was recorded with. The layer paint carries globalAlpha and
    // the composite op, which the spec applies after the filter.
    c->save();
    c->resetMatrix();
    SkPaint layer;
    layer.setImageFilter(filter);
    layer.setAlphaf(cmd.layerAlpha);
    layer.setBlendMode(cmd.layerBlend);
    c->saveLayer(nullptr, &layer);
    c->setMatrix(ctm);
    replayOne(c, cmd);
    c->restore();
    c->restore();
}

void CanvasScene::replayCommands(SkCanvas* c, std::vector<CanvasCmd>& cmds) {
    for (auto& cmd : cmds) {
        if (!cmd.filter && !cmd.shadow) {
            replayOne(c, cmd);
            continue;
        }
        const SkM44 ctm = c->getLocalToDevice();
        // The spec's drawing model: the shadow (of the filtered image, when
        // there is a filter) is composited first, with globalAlpha and the
        // composite op of its own, and then the shape is, with the same two.
        if (cmd.shadow) replayThroughFilter(c, cmd, ctm, cmd.shadow);
        if (cmd.filter) {
            replayThroughFilter(c, cmd, ctm, cmd.filter);
        } else {
            // Shadow but no filter: the shape needs no layer, so globalAlpha
            // and the composite op go back onto its own paint — exactly the
            // paint an unshadowed draw would have carried.
            const SkPaint recorded = cmd.paint;
            cmd.paint.setAlphaf(recorded.getAlphaf() * cmd.layerAlpha);
            cmd.paint.setBlendMode(cmd.layerBlend);
            replayOne(c, cmd);
            cmd.paint = recorded;
        }
    }
}

void CanvasScene::flushStagedCommands() {
    if (stagedCommands_.empty()) return;

    if (tryStreamingPutImageDataFastPath(surface_.get(), stagedCommands_)) {
        stagedCommands_.clear();
        return;
    }

    auto* c = skCanvas();
    if (!c) { stagedCommands_.clear(); return; }

    replayCommands(c, stagedCommands_);
    stagedCommands_.clear();
}

// ---------------------------------------------------------------------------
// Command buffer replay
// ---------------------------------------------------------------------------

void CanvasScene::flushCommands() {
    if (commands_.empty()) return;

    if (tryStreamingPutImageDataFastPath(surface_.get(), commands_)) {
        commands_.clear();
        return;
    }

    auto* c = skCanvas();
    if (!c) { commands_.clear(); return; }

    replayCommands(c, commands_);
    commands_.clear();
    dirty_ = true;
    snapshotValid_ = false;
    snapshotImageValid_ = false;
}

// ---------------------------------------------------------------------------
// Compositing — bring the surface up to date for this frame
// ---------------------------------------------------------------------------

void CanvasScene::rasterize() {
    // Was the element removed from the DOM? An offscreen canvas (created and
    // never appended, a sprite atlas say) reads as orphaned from frame one, so
    // only one that has been seen attached counts as detached. The backing
    // Element may have been freed since the scene was last touched;
    // backingElementAlive() checks the pointer's value without dereferencing it.
    if (detachedCb_) {
        if (!backingElementAlive()) { onElementFinalized(); return; }
        bool orphaned = detachedCb_(detachedUd_);
        if (!orphaned) everAttached_ = true;
        if (orphaned && everAttached_) {
            detached_ = true;
            return;
        }
    }

    // Query element layout position (display rect). Surface size comes from
    // queryLayoutWidth/Height so intrinsic canvas.width/height beats layout.
    float layoutX = 0, layoutY = 0, layoutW = 0, layoutH = 0;
    if (layoutCb_) {
        layoutCb_(layoutUd_, layoutX, layoutY, layoutW, layoutH);
    }

    screenX_ = layoutX;
    screenY_ = layoutY - viewportScrollY_;

    int canvasW = queryLayoutWidth();
    int canvasH = queryLayoutHeight();
    if (canvasW <= 0 || canvasH <= 0) return;
    ensureSurface(canvasW, canvasH);

    flushCommands();
    dirty_ = false;
}

} // namespace bro::canvas
