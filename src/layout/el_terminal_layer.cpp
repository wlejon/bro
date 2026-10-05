// ElTerminal's paint: drawn inline (draw()) or recorded into the terminal's
// own compositor layer (recordLayer(); see term_layer.h for who replays it).

#include "layout/el_terminal_impl.h"

#include "dom/element.h"
#include "render/recording_renderer.h"

#include <atomic>
#include <cmath>

namespace bro::layout {

namespace {
std::atomic<uint64_t> g_totalRecords{0};
} // namespace

std::shared_ptr<TermLayer> ElTerminal::layer() const { return impl_->layer; }
uint64_t ElTerminal::layerRecords() const { return impl_->records; }
uint64_t ElTerminal::totalLayerRecords() { return g_totalRecords.load(std::memory_order_relaxed); }

void ElTerminal::draw(render::Renderer* renderer, float x, float y, float w, float h) {
    Impl& m = *impl_;
    if (!m.frame) m.frame = m.session->acquireFrame();
    if (!m.frame || !renderer || w <= 0 || h <= 0) return;
    m.layerDirty = false;  // what is shown has been painted (inline or into the layer)
    terminal::PaintOptions opts;
    opts.font = m.font();  // family views m.family, stable for the paint
    opts.focused = m.focused;
    opts.blinkOn = m.blinkOn;
    opts.preedit = preedit_;
    opts.colors = m.colors;
    opts.highlights = m.highlights;
    opts.ligatures = options_.ligatures;
    m.painter.paint(renderer, *m.frame, x, y, w, h, m.cellMetrics(), opts);
}

bool ElTerminal::recordLayer(float scale) {
    Impl& m = *impl_;
    if (!elem_ || !renderer_) return false;
    const auto& box = elem_->layoutBox();
    const int w = int(std::ceil(box.contentRect.width));
    const int h = int(std::ceil(box.contentRect.height));
    TermLayer& layer = *m.layer;
    if (!m.layerDirty && layer.boxW == w && layer.boxH == h && layer.scale == scale &&
        layer.commands.commandCount() > 0)
        return false;
    m.layerDirty = false;
    layer.boxW = w;
    layer.boxH = h;
    layer.scale = scale;
    layer.commands.clear();
    if (w > 0 && h > 0) {
        render::RecordingRenderer rec(&layer.commands, renderer_);
        draw(&rec, 0.0f, 0.0f, box.contentRect.width, box.contentRect.height);
    }
    ++layer.generation;
    ++m.records;
    g_totalRecords.fetch_add(1, std::memory_order_relaxed);
    return true;
}

} // namespace bro::layout
