// TermPainter's images: bropty's Frame::images drawn on their planes.

#include "terminal/term_paint.h"

namespace bro::terminal {

TermPainter::ImageRect TermPainter::imageRect(const bropty::FrameImage& im, float x, float y, const CellMetrics& m) {
    // bropty places images in (fractional) cells of its own image cell size,
    // which is the cell this painter draws (the session gives the terminal
    // pixelWidth / pixelHeight): positions and sizes scale with the cell, so
    // an image covers exactly the cells it covers in the terminal.
    return ImageRect{x + im.x * m.cellW, y + im.y * m.cellH, im.w * m.cellW, im.h * m.cellH};
}

void TermPainter::drawImages(render::Renderer* r, const bropty::Frame& f, bropty::ImagePlane plane, float x, float y,
                             const CellMetrics& m) {
    for (const bropty::FrameImage& im : f.images) {
        if (im.plane != plane || !im.pixels || im.pixels->width == 0 || im.pixels->height == 0) continue;
        const bropty::ImagePixels& px = *im.pixels;
        if (px.rgba.size() < size_t(px.width) * px.height * 4) continue;
        render::SharedPixels sp;
        sp.id = px.serial;
        sp.width = int(px.width);
        sp.height = int(px.height);
        sp.rgba = px.rgba.data();
        sp.owner = im.pixels;
        const ImageRect d = imageRect(im, x, y, m);
        r->drawSharedPixels(sp, im.src_x, im.src_y, im.src_w, im.src_h, d.x, d.y, d.w, d.h);
    }
}

} // namespace bro::terminal
