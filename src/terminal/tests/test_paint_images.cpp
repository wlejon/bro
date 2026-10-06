// Inline images through the painter: what TermPainter draws checked against
// bropty's own image state (placements, image cells, the frame's image
// list), for synthetic kitty / sixel transmissions and for the captured
// output of real programs (kitten icat, img2sixel, chafa, imgcat), decoded
// by the session's own decoder (broimage). Positions are checked at a cell
// size other than the one the terminal laid the images out with, as an
// element at another font size or device scale has.

#include "check.h"
#include "paint_fixture.h"
#include "tests.h"

#include "terminal/term_session_host.h"

#include <broimage/decode.h>
#include <broimage/encode.h>

#include <bropty/graphics.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace bro::terminal::test {

namespace {

// The terminal lays images out in 10 x 20 px cells; the painter draws 8.5 x
// 17 CSS px cells (fixedMetrics), so every position is a cell conversion.
constexpr int kPxW = 10, kPxH = 20;

std::string b64(std::string_view in) {
    static const char* tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
        for (int k = 3; k >= 0; --k) out.push_back(tab[(v >> (6 * k)) & 63]);
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size()) v |= uint32_t(uint8_t(in[i + 1])) << 8;
        out.push_back(tab[(v >> 18) & 63]);
        out.push_back(tab[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? tab[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::string kitty(const std::string& control, std::string_view payload = {}) {
    return "\x1b_G" + control + (payload.empty() ? "" : ";" + b64(payload)) + "\x1b\\";
}

// An RGBA test pattern, w x h.
std::string pattern(int w, int h, int seed) {
    std::string px(size_t(w) * h * 4, '\0');
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            char* p = px.data() + (size_t(y) * w + x) * 4;
            p[0] = char(x * 9 + seed), p[1] = char(y * 13 + seed * 3), p[2] = char((x ^ y) * 5), p[3] = char(255);
        }
    return px;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string dataPath(const std::string& name) { return std::string(BROPTY_TEST_DATA) + "/images/" + name; }

// The session's decoder behind a bare Terminal.
struct DecodeHost : bropty::TerminalHost {
    int decodes = 0;
    bool decode_image(std::string_view data, const bropty::ImageLimits& limits, bropty::DecodedImage& out) override {
        ++decodes;
        return decodeTerminalImage(data, limits, out);
    }
};

struct ImgFixture {
    DecodeHost host;
    std::unique_ptr<bropty::Terminal> term;
    std::unique_ptr<bropty::TerminalView> view;
    ImgFixture(int cols, int rows, size_t scrollback = 100) {
        term = std::make_unique<bropty::Terminal>(cols, rows, scrollback);
        term->set_host(&host);
        term->set_cell_pixel_size(kPxW, kPxH);
        view = std::make_unique<bropty::TerminalView>(*term);
    }
    void feed(std::string_view s) { term->feed(s); }
    std::shared_ptr<const bropty::Frame> frame() { return view->snapshot(); }
};

std::vector<Op> paint(const bropty::Frame& f) {
    CaptureRenderer r;
    TermPainter p;
    PaintOptions o;
    o.font = fixedFont();
    p.paint(&r, f, kOriginX, kOriginY, 1000, 1000, fixedMetrics(), o);
    return std::move(r.ops);
}

std::vector<const Op*> imageOps(const std::vector<Op>& ops) {
    std::vector<const Op*> v;
    for (const Op& o : ops)
        if (o.kind == Op::Image) v.push_back(&o);
    return v;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

std::string rect(float x, float y, float w, float h) {
    char b[96];
    std::snprintf(b, sizeof b, "(%.3f %.3f %.3f %.3f)", double(x), double(y), double(w), double(h));
    return b;
}

// Every frame image is drawn once, in the frame's order within its plane,
// at its cells (converted to the painter's cells), with its source rect and
// pixels; and nothing else is drawn as an image.
void checkFrameImagesPainted(const bropty::Frame& f, const std::vector<Op>& ops) {
    const CellMetrics m = fixedMetrics();
    std::vector<const bropty::FrameImage*> want;
    for (auto plane : {bropty::ImagePlane::BelowBackground, bropty::ImagePlane::BelowText, bropty::ImagePlane::Text,
                       bropty::ImagePlane::AboveText})
        for (const bropty::FrameImage& im : f.images)
            if (im.plane == plane) want.push_back(&im);
    const auto got = imageOps(ops);
    CHECK_MSG(got.size() == want.size(), std::to_string(got.size()) + " drawn, " + std::to_string(want.size()) + " in the frame");
    for (size_t i = 0; i < std::min(got.size(), want.size()); ++i) {
        const bropty::FrameImage& im = *want[i];
        const Op& op = *got[i];
        const float x = kOriginX + im.x * m.cellW, y = kOriginY + im.y * m.cellH;
        CHECK_MSG(near(op.x, x) && near(op.y, y) && near(op.w, im.w * m.cellW) && near(op.h, im.h * m.cellH),
                  rect(op.x, op.y, op.w, op.h) + " want " + rect(x, y, im.w * m.cellW, im.h * m.cellH));
        CHECK(op.sx == im.src_x && op.sy == im.src_y && op.sw == im.src_w && op.sh == im.src_h);
        CHECK(op.pixelsId == im.pixels->serial && op.pixels == im.pixels->rgba.data());
        CHECK(op.pixelsW == int(im.pixels->width) && op.pixelsH == int(im.pixels->height));
    }
}

// A kitty placement's box in the painter's CSS px, from bropty's placement
// state (not from the frame): its top-left cell and pixel offset, and its
// size from c / r or, when not given, the source rect at the terminal's
// cell size. Rows relative to the frame's top row.
struct Box {
    float x, y, w, h;
};
Box placementBox(const bropty::Terminal& t, const bropty::Placement& p, int64_t topRow) {
    const CellMetrics m = fixedMetrics();
    int64_t row = 0;
    int col = 0;
    t.images().position(p, row, col);
    const bropty::Image* img = t.images().by_key(p.image_key);
    const float sw = float(p.src_w ? p.src_w : img->width - p.src_x);
    const float sh = float(p.src_h ? p.src_h : img->height - p.src_y);
    Box b;
    b.x = kOriginX + (float(col) + float(p.x_offset) / kPxW) * m.cellW;
    b.y = kOriginY + (float(row - topRow) + float(p.y_offset) / kPxH) * m.cellH;
    if (p.cols > 0 && p.rows > 0) {
        b.w = float(p.cols) * m.cellW;
        b.h = float(p.rows) * m.cellH;
    } else if (p.cols > 0) {
        b.w = float(p.cols) * m.cellW;
        b.h = b.w / m.cellW * kPxW * sh / sw / kPxH * m.cellH;
    } else if (p.rows > 0) {
        b.h = float(p.rows) * m.cellH;
        b.w = b.h / m.cellH * kPxH * sw / sh / kPxW * m.cellW;
    } else {
        b.w = sw / kPxW * m.cellW;
        b.h = sh / kPxH * m.cellH;
    }
    return b;
}

void checkPlacementDrawn(const bropty::Terminal& t, const bropty::Placement& p, const bropty::Frame& f,
                         const std::vector<Op>& ops) {
    const Box b = placementBox(t, p, f.top_row);
    const bropty::Image* img = t.images().by_key(p.image_key);
    bool found = false;
    std::string seen;
    for (const Op* op : imageOps(ops)) {
        if (!img || op->pixelsId != img->pixels()->serial) continue;
        seen += rect(op->x, op->y, op->w, op->h) + " ";
        found = found || (near(op->x, b.x) && near(op->y, b.y) && near(op->w, b.w) && near(op->h, b.h));
    }
    CHECK_MSG(found, "placement " + std::to_string(p.placement_id) + " of image " + std::to_string(p.image_id) +
                         " drawn at " + rect(b.x, b.y, b.w, b.h) + "; drawn: " + seen);
}

// Index of the first / last op of a kind (-1: none).
int firstOf(const std::vector<Op>& ops, Op::Kind k, size_t from = 0) {
    for (size_t i = from; i < ops.size(); ++i)
        if (ops[i].kind == k) return int(i);
    return -1;
}
int lastOf(const std::vector<Op>& ops, Op::Kind k) {
    for (size_t i = ops.size(); i-- > 0;)
        if (ops[i].kind == k) return int(i);
    return -1;
}

// ---------------------------------------------------------------------------

void testKittyPlacement() {
    section("images: kitty placement");
    ImgFixture fx(40, 12);
    const std::string px = pattern(23, 31, 1);
    // Natural size at (3, 5) with a pixel offset; then c/r-sized at (6, 0).
    fx.feed("\x1b[4;6H");
    fx.feed(kitty("a=T,f=32,s=23,v=31,i=1,X=3,Y=7,C=1,q=2", px));
    fx.feed("\x1b[7;1H");
    fx.feed(kitty("a=p,i=1,p=2,c=5,r=2,C=1,q=2"));
    const auto f = fx.frame();
    CHECK(f->images.size() == 2);
    CHECK(fx.term->images().placements().size() == 2);
    const auto ops = paint(*f);
    checkFrameImagesPainted(*f, ops);
    for (const bropty::Placement& p : fx.term->images().placements()) checkPlacementDrawn(*fx.term, p, *f, ops);
    // The pixels are the transmitted ones, by reference.
    const auto got = imageOps(ops);
    if (!got.empty())
        CHECK(std::string(reinterpret_cast<const char*>(got[0]->pixels), px.size()) == px);
}

void testKittyPlanes() {
    section("images: kitty z planes");
    ImgFixture fx(30, 8);
    const std::string px = pattern(8, 8, 2);
    // A coloured background cell and a glyph under all three.
    fx.feed("\x1b[41mX\x1b[m\r");
    fx.feed(kitty("a=t,f=32,s=8,v=8,i=7,q=2", px));
    fx.feed(kitty("a=p,i=7,p=1,z=-1073741825,C=1,q=2"));  // under the cell backgrounds
    fx.feed(kitty("a=p,i=7,p=2,z=-5,C=1,q=2"));           // between backgrounds and text
    fx.feed(kitty("a=p,i=7,p=3,z=4,C=1,q=2"));            // over the text
    const auto f = fx.frame();
    CHECK(f->images.size() == 3);
    const auto ops = paint(*f);
    checkFrameImagesPainted(*f, ops);
    // ops[0..2] are save / clip / the default background.
    const int bgFill = firstOf(ops, Op::Fill, 3);
    const int text = firstOf(ops, Op::Text);
    std::vector<int> imgAt;
    for (size_t i = 0; i < ops.size(); ++i)
        if (ops[i].kind == Op::Image) imgAt.push_back(int(i));
    CHECK(imgAt.size() == 3 && bgFill > 0 && text > 0);
    if (imgAt.size() == 3 && bgFill > 0 && text > 0) {
        CHECK_MSG(imgAt[0] < bgFill, "z < INT32_MIN/2 under the cell background");
        CHECK_MSG(imgAt[1] > bgFill && imgAt[1] < text, "negative z between background and text");
        CHECK_MSG(imgAt[2] > lastOf(ops, Op::Text), "z >= 0 over the text");
    }
}

void testScrollAndEviction() {
    section("images: scroll with the text, leave with history");
    ImgFixture fx(20, 6, /*scrollback=*/10);
    const std::string px = pattern(20, 40, 3);  // 2 x 2 cells
    fx.feed("\x1b[3;4H");
    fx.feed(kitty("a=T,f=32,s=20,v=40,i=3,C=1,q=2", px));
    auto f = fx.frame();
    CHECK(f->images.size() == 1 && f->images[0].y == 2.0f);
    // Three lines of output scroll it up three rows, half into history.
    fx.feed("\x1b[6;1H\n\n\n");
    f = fx.frame();
    CHECK(f->images.size() == 1);
    if (!f->images.empty()) CHECK(f->images[0].y == -1.0f);  // its lower half still on screen
    auto ops = paint(*f);
    checkFrameImagesPainted(*f, ops);
    for (const bropty::Placement& p : fx.term->images().placements()) checkPlacementDrawn(*fx.term, p, *f, ops);
    // Scrolled back, it is where it was written.
    fx.view->scroll_by(-3);
    f = fx.frame();
    CHECK(f->images.size() == 1 && !f->at_bottom());
    if (!f->images.empty()) CHECK(f->images[0].y == 2.0f);
    ops = paint(*f);
    checkFrameImagesPainted(*f, ops);
    for (const bropty::Placement& p : fx.term->images().placements()) checkPlacementDrawn(*fx.term, p, *f, ops);
    // Its rows evicted from history (10 rows held): gone from the terminal
    // and from every view of it.
    fx.view->scroll_to_bottom();
    for (int i = 0; i < 20; ++i) fx.feed("line\r\n");
    CHECK(fx.term->images().placements().empty());
    fx.view->scroll_to_row(fx.term->first_row());
    f = fx.frame();
    CHECK(f->images.empty());
    CHECK(imageOps(paint(*f)).empty());
}

void testSixelCells() {
    section("images: sixel cells");
    ImgFixture fx(30, 10);
    // 12 x 24 px of red: 2 x 2 cells at 10 x 20 px (ceil(1.2) x ceil(1.2)).
    std::string six = "\x1bPq#1;2;100;0;0#1";
    for (int band = 0; band < 4; ++band) six += "!12~" + std::string(band < 3 ? "-" : "");
    six += "\x1b\\";
    fx.feed("\x1b[2;3H");
    fx.feed(six);
    // The cursor is left on the image's last row (xterm): text goes below it.
    fx.feed("\r\nAB");
    const auto f = fx.frame();
    const auto ops = paint(*f);
    checkFrameImagesPainted(*f, ops);
    CHECK(!f->images.empty());
    // The images cover the placeholder cells' box and nothing is drawn as
    // a glyph there.
    const CellMetrics m = fixedMetrics();
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f, srcArea = 0;
    for (const bropty::FrameImage& im : f->images) {
        CHECK(im.plane == bropty::ImagePlane::Text && im.source == bropty::ImageSource::Sixel);
        x0 = std::min(x0, im.x), y0 = std::min(y0, im.y);
        x1 = std::max(x1, im.x + im.w), y1 = std::max(y1, im.y + im.h);
        srcArea += im.src_w * im.src_h;
    }
    CHECK_MSG(x0 == 2.0f && y0 == 1.0f, rect(x0, y0, x1 - x0, y1 - y0));
    const bropty::Image* img = nullptr;
    fx.term->images().for_each_image([&](const bropty::Image& i) { img = &i; });
    CHECK(img != nullptr);
    if (img) {
        // The image's extent in cells, at the terminal's 10 x 20 px cells,
        // and every source pixel drawn once.
        CHECK_MSG(near(x1 - x0, float(img->width) / kPxW) && near(y1 - y0, float(img->height) / kPxH),
                  rect(x0, y0, x1 - x0, y1 - y0) + " for " + std::to_string(img->width) + "x" +
                      std::to_string(img->height));
        CHECK(near(srcArea, float(img->width * img->height), 0.5f));
        CHECK(img->width == 12);
    }
    bool textAB = false;
    for (const Op& op : ops) {
        if (op.kind != Op::Text) continue;
        CHECK_MSG(op.text.find("\xF4\x8E\xBB\xAE") == std::string::npos, "no placeholder glyph drawn");
        textAB = textAB || op.text == "AB";
    }
    CHECK_MSG(textAB, "text after the image is drawn");
    (void)m;
}

void testDecodeLimits() {
    section("images: the decoder respects bropty's limits");
    std::vector<uint8_t> png;
    const std::string px = pattern(40, 30, 4);
    CHECK(broimage::encode_png_memory(png, reinterpret_cast<const uint8_t*>(px.data()), 40, 30, 4));
    const std::string_view data(reinterpret_cast<const char*>(png.data()), png.size());
    bropty::DecodedImage out;
    bropty::ImageLimits lim{100, 100, 40 * 30 * 4};
    CHECK(decodeTerminalImage(data, lim, out));
    CHECK(out.width == 40 && out.height == 30 && out.frames.size() == 1 &&
          std::string(reinterpret_cast<const char*>(out.frames[0].rgba.data()), out.frames[0].rgba.size()) == px);
    CHECK(!decodeTerminalImage(data, bropty::ImageLimits{39, 100, 1 << 20}, out));
    CHECK(!decodeTerminalImage(data, bropty::ImageLimits{100, 29, 1 << 20}, out));
    CHECK(!decodeTerminalImage(data, bropty::ImageLimits{100, 100, 40 * 30 * 4 - 1}, out));
    CHECK(!decodeTerminalImage(data, bropty::ImageLimits{0, 0, 0}, out));
    // Through the terminal: a kitty PNG (f=100) larger than the quota is
    // refused (ENOSPC), and the quota holds.
    bropty::TerminalOptions o;
    o.cols = 20, o.rows = 5;
    o.graphics.storage_limit = 40 * 30 * 4 - 1;
    bropty::Terminal t(o);
    DecodeHost host;
    t.set_host(&host);
    t.feed(kitty("a=t,f=100,i=1", data));
    CHECK(t.images().kitty_image_count() == 0 && t.image_bytes() == 0);
}

// ---- real programs (bropty's captures) -----------------------------------------

struct Rgba {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};
Rgba sourceImage(const char* name) {
    Rgba r;
    broimage::Image img;
    const std::string bytes = readFile(dataPath(name));
    if (broimage::decode_memory(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), img)) {
        r.w = img.width, r.h = img.height, r.px = std::move(img.pixels);
    }
    return r;
}

bool samePixels(const bropty::ImagePixels& p, const Rgba& r) {
    return int(p.width) == r.w && int(p.height) == r.h && p.rgba == r.px;
}

void testPrograms() {
    section("images: real programs (captured output)");
    struct Case {
        const char* name;
        const char* source;   // the PNG it showed, when shown losslessly
        int wantImages;
    };
    const Case cases[] = {
        {"icat", "src_a.png", 1},          {"icat_place", "src_b.png", 1},
        {"icat_placeholder", "src_a.png", 1}, {"img2sixel", nullptr, 1},
        {"chafa_sixel", nullptr, 1},       {"chafa_kitty", nullptr, 1},
        {"chafa_iterm", nullptr, 1},       {"imgcat", "src_b.png", 1},
    };
    for (const Case& c : cases) {
        const std::string bytes = readFile(dataPath(std::string("programs/") + c.name + ".vt"));
        CHECK_MSG(!bytes.empty(), std::string("capture ") + c.name);
        if (bytes.empty()) continue;
        ImgFixture fx(80, 24);
        fx.feed(bytes);
        size_t count = 0;
        const bropty::Image* first = nullptr;
        fx.term->images().for_each_image([&](const bropty::Image& i) {
            ++count;
            if (!first) first = &i;
        });
        CHECK_MSG(int(count) == c.wantImages, std::string(c.name) + ": " + std::to_string(count) + " images");
        if (c.source && first) {
            const Rgba src = sourceImage(c.source);
            CHECK_MSG(samePixels(*first->pixels(), src), std::string(c.name) + ": decoded pixels are the source's");
        }
        const auto f = fx.frame();
        CHECK_MSG(!f->images.empty(), std::string(c.name) + ": shown");
        const auto ops = paint(*f);
        checkFrameImagesPainted(*f, ops);
        for (const bropty::Placement& p : fx.term->images().placements())
            if (!p.is_virtual) checkPlacementDrawn(*fx.term, p, *f, ops);
        // What uses the host's decoder decoded through it.
        if (std::string(c.name) == "icat" || std::string(c.name) == "imgcat" || std::string(c.name) == "chafa_iterm")
            CHECK_MSG(fx.host.decodes >= 1, std::string(c.name) + ": decoded by the host");
    }
}

} // namespace

void run_paint_image_tests() {
    testKittyPlacement();
    testKittyPlanes();
    testScrollAndEviction();
    testSixelCells();
    testDecodeLimits();
    testPrograms();
}

} // namespace bro::terminal::test
