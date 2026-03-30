#include "layout/container.h"
#include "layout/bro_element.h"
#include "layout/el_input.h"
#include "layout/el_textarea.h"
#include "layout/el_select.h"
#include "layout/el_svg.h"
#include "render/renderer.h"
#include "util/log.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stb_image.h>

namespace bro::layout {

BroContainer::BroContainer(render::Renderer* renderer, int viewportWidth, int viewportHeight)
    : renderer_(renderer)
    , viewportWidth_(viewportWidth)
    , viewportHeight_(viewportHeight) {
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

litehtml::uint_ptr BroContainer::create_font(const litehtml::font_description& descr,
                                              const litehtml::document* /*doc*/,
                                              litehtml::font_metrics* fm) {
    bool italic = (descr.style == litehtml::font_style_italic);
    uint64_t handle = fontManager_.createFont(
        renderer_, descr.family,
        static_cast<float>(descr.size),
        descr.weight, italic);

    if (fm) {
        auto m = fontManager_.getMetrics(handle);
        fm->font_size  = descr.size;
        fm->height     = static_cast<litehtml::pixel_t>(m.height);
        fm->ascent     = static_cast<litehtml::pixel_t>(m.ascent);
        fm->descent    = static_cast<litehtml::pixel_t>(m.descent);
        fm->x_height   = static_cast<litehtml::pixel_t>(m.x_height);
        fm->draw_spaces = true;
    }
    return static_cast<litehtml::uint_ptr>(handle);
}

void BroContainer::delete_font(litehtml::uint_ptr hFont) {
    fontManager_.deleteFont(renderer_, static_cast<uint64_t>(hFont));
}

litehtml::pixel_t BroContainer::text_width(const char* text, litehtml::uint_ptr hFont) {
    auto tm = renderer_->measureText(text, static_cast<uint64_t>(hFont));
    return static_cast<litehtml::pixel_t>(tm.width);
}

void BroContainer::draw_text(litehtml::uint_ptr /*hdc*/, const char* text,
                              litehtml::uint_ptr hFont,
                              litehtml::web_color color,
                              const litehtml::position& pos) {
    // Skip whitespace-only text nodes (litehtml generates many between elements)
    if (!text || !*text) return;
    const char* p = text;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p) return;

    drawStats.texts++;
    render::Color c{color.red, color.green, color.blue, color.alpha};
    // litehtml gives pos.y as the top of the text box, but Skia's
    // drawSimpleText expects the baseline y. Offset by the font ascent.
    uint64_t handle = static_cast<uint64_t>(hFont);
    auto metrics = fontManager_.getMetrics(handle);
    renderer_->drawText(text,
                        static_cast<float>(pos.x),
                        static_cast<float>(pos.y) + metrics.ascent,
                        handle, c);
}

litehtml::pixel_t BroContainer::pt_to_px(float pt) const {
    return static_cast<litehtml::pixel_t>(pt * 96.0f / 72.0f);
}

litehtml::pixel_t BroContainer::get_default_font_size() const {
    return 16;
}

const char* BroContainer::get_default_font_name() const {
    return "Arial";
}

// ---------------------------------------------------------------------------
// List markers
// ---------------------------------------------------------------------------

void BroContainer::draw_list_marker(litehtml::uint_ptr /*hdc*/,
                                     const litehtml::list_marker& marker) {
    render::Color c{marker.color.red, marker.color.green, marker.color.blue, marker.color.alpha};
    // Draw a small filled square as the marker.
    float size = 6.0f;
    float x = static_cast<float>(marker.pos.x);
    float y = static_cast<float>(marker.pos.y) + static_cast<float>(marker.pos.height) / 2.0f - size / 2.0f;
    renderer_->fillRect(x, y, size, size, c);
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------

void BroContainer::load_image(const char* src, const char* baseurl, bool /*redraw_on_ready*/) {
    if (!src || !*src) return;
    std::string url = src;
    if (imageCache_.count(url)) return; // already loaded

    // Resolve path relative to base URL or app base
    std::string base = (baseurl && *baseurl) ? baseurl : baseUrl_;
    std::string path;
    // Check if already absolute
    if (url.size() >= 2 && url[1] == ':') {
        path = url;
    } else if (!url.empty() && (url[0] == '/' || url[0] == '\\')) {
        path = url;
    } else if (!base.empty()) {
        path = base;
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += url;
    } else {
        path = url;
    }

    // Read entire file into memory
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        LOG_WARN("load_image: failed to open '%s'", path.c_str());
        return;
    }
    auto fileSize = ifs.tellg();
    ifs.seekg(0);
    CachedImage img;
    img.data.resize(static_cast<size_t>(fileSize));
    ifs.read(reinterpret_cast<char*>(img.data.data()), fileSize);

    // Query dimensions via stb_image without full decode
    int w = 0, h = 0, comp = 0;
    if (stbi_info_from_memory(img.data.data(), static_cast<int>(img.data.size()), &w, &h, &comp)) {
        img.width = w;
        img.height = h;
    }
    imageCache_[url] = std::move(img);
}

void BroContainer::get_image_size(const char* src, const char* /*baseurl*/, litehtml::size& sz) {
    sz.width = 0;
    sz.height = 0;
    if (!src) return;
    auto it = imageCache_.find(src);
    if (it != imageCache_.end()) {
        sz.width = it->second.width;
        sz.height = it->second.height;
    }
}

void BroContainer::draw_image(litehtml::uint_ptr /*hdc*/,
                               const litehtml::background_layer& layer,
                               const std::string& url,
                               const std::string& /*base_url*/) {
    auto it = imageCache_.find(url);
    if (it == imageCache_.end() || it->second.data.empty()) return;
    renderer_->drawImage(it->second.data.data(), it->second.data.size(),
                         static_cast<float>(layer.border_box.x),
                         static_cast<float>(layer.border_box.y),
                         static_cast<float>(layer.border_box.width),
                         static_cast<float>(layer.border_box.height));
}

// ---------------------------------------------------------------------------
// Backgrounds
// ---------------------------------------------------------------------------

void BroContainer::draw_solid_fill(litehtml::uint_ptr /*hdc*/,
                                    const litehtml::background_layer& layer,
                                    const litehtml::web_color& color) {
    if (color.alpha == 0) return; // transparent — let scene show through


    drawStats.fills++;
    render::Color c{color.red, color.green, color.blue, color.alpha};
    renderer_->fillRect(static_cast<float>(layer.border_box.x),
                        static_cast<float>(layer.border_box.y),
                        static_cast<float>(layer.border_box.width),
                        static_cast<float>(layer.border_box.height), c);
}

void BroContainer::draw_linear_gradient(litehtml::uint_ptr /*hdc*/,
                                         const litehtml::background_layer& layer,
                                         const litehtml::background_layer::linear_gradient& gradient) {
    if (gradient.color_points.empty()) return;
    float bx = static_cast<float>(layer.border_box.x);
    float by = static_cast<float>(layer.border_box.y);
    float bw = static_cast<float>(layer.border_box.width);
    float bh = static_cast<float>(layer.border_box.height);

    std::vector<render::ColorStop> stops;
    stops.reserve(gradient.color_points.size());
    for (auto& cp : gradient.color_points) {
        stops.push_back({cp.offset,
            {cp.color.red, cp.color.green, cp.color.blue, cp.color.alpha}});
    }
    renderer_->fillLinearGradient(bx, by, bw, bh,
                                  gradient.start.x, gradient.start.y,
                                  gradient.end.x, gradient.end.y,
                                  stops);
}

void BroContainer::draw_radial_gradient(litehtml::uint_ptr /*hdc*/,
                                         const litehtml::background_layer& layer,
                                         const litehtml::background_layer::radial_gradient& gradient) {
    if (gradient.color_points.empty()) return;
    float bx = static_cast<float>(layer.border_box.x);
    float by = static_cast<float>(layer.border_box.y);
    float bw = static_cast<float>(layer.border_box.width);
    float bh = static_cast<float>(layer.border_box.height);

    std::vector<render::ColorStop> stops;
    stops.reserve(gradient.color_points.size());
    for (auto& cp : gradient.color_points) {
        stops.push_back({cp.offset,
            {cp.color.red, cp.color.green, cp.color.blue, cp.color.alpha}});
    }
    renderer_->fillRadialGradient(bx, by, bw, bh,
                                  gradient.position.x, gradient.position.y,
                                  gradient.radius.x, gradient.radius.y,
                                  stops);
}

void BroContainer::draw_conic_gradient(litehtml::uint_ptr /*hdc*/,
                                        const litehtml::background_layer& layer,
                                        const litehtml::background_layer::conic_gradient& gradient) {
    if (gradient.color_points.empty()) return;
    float bx = static_cast<float>(layer.border_box.x);
    float by = static_cast<float>(layer.border_box.y);
    float bw = static_cast<float>(layer.border_box.width);
    float bh = static_cast<float>(layer.border_box.height);

    std::vector<render::ColorStop> stops;
    stops.reserve(gradient.color_points.size());
    for (auto& cp : gradient.color_points) {
        stops.push_back({cp.offset,
            {cp.color.red, cp.color.green, cp.color.blue, cp.color.alpha}});
    }
    renderer_->fillConicGradient(bx, by, bw, bh,
                                 gradient.position.x, gradient.position.y,
                                 gradient.angle,
                                 stops);
}

// ---------------------------------------------------------------------------
// Borders
// ---------------------------------------------------------------------------

void BroContainer::draw_borders(litehtml::uint_ptr /*hdc*/,
                                 const litehtml::borders& borders,
                                 const litehtml::position& draw_pos,
                                 bool /*root*/) {
    drawStats.borders++;
    float x = static_cast<float>(draw_pos.x);
    float y = static_cast<float>(draw_pos.y);
    float w = static_cast<float>(draw_pos.width);
    float h = static_cast<float>(draw_pos.height);

    // Top border
    if (borders.top.width > 0 && borders.top.style != litehtml::border_style_none) {
        render::Color c{borders.top.color.red, borders.top.color.green,
                        borders.top.color.blue, borders.top.color.alpha};
        renderer_->drawLine(x, y, x + w, y, c, static_cast<float>(borders.top.width));
    }
    // Bottom border
    if (borders.bottom.width > 0 && borders.bottom.style != litehtml::border_style_none) {
        render::Color c{borders.bottom.color.red, borders.bottom.color.green,
                        borders.bottom.color.blue, borders.bottom.color.alpha};
        renderer_->drawLine(x, y + h, x + w, y + h, c, static_cast<float>(borders.bottom.width));
    }
    // Left border
    if (borders.left.width > 0 && borders.left.style != litehtml::border_style_none) {
        render::Color c{borders.left.color.red, borders.left.color.green,
                        borders.left.color.blue, borders.left.color.alpha};
        renderer_->drawLine(x, y, x, y + h, c, static_cast<float>(borders.left.width));
    }
    // Right border
    if (borders.right.width > 0 && borders.right.style != litehtml::border_style_none) {
        render::Color c{borders.right.color.red, borders.right.color.green,
                        borders.right.color.blue, borders.right.color.alpha};
        renderer_->drawLine(x + w, y, x + w, y + h, c, static_cast<float>(borders.right.width));
    }
}

// ---------------------------------------------------------------------------
// Document metadata
// ---------------------------------------------------------------------------

void BroContainer::set_caption(const char* caption) {
    caption_ = caption ? caption : "";
}

void BroContainer::set_base_url(const char* base_url) {
    baseUrl_ = base_url ? base_url : "";
}

void BroContainer::link(const std::shared_ptr<litehtml::document>& /*doc*/,
                         const litehtml::element::ptr& /*el*/) {
    // Stub: link element handling not implemented.
}

void BroContainer::on_anchor_click(const char* url, const litehtml::element::ptr& /*el*/) {
    LOG_INFO("Anchor clicked: %s", url ? url : "(null)");
}

void BroContainer::on_mouse_event(const litehtml::element::ptr& /*el*/,
                                   litehtml::mouse_event /*event*/) {
    // Stub.
}

void BroContainer::set_cursor(const char* cursor) {
    cursor_ = cursor ? cursor : "";
}

// ---------------------------------------------------------------------------
// Text transforms
// ---------------------------------------------------------------------------

void BroContainer::transform_text(litehtml::string& text, litehtml::text_transform tt) {
    switch (tt) {
        case litehtml::text_transform_uppercase:
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            break;
        case litehtml::text_transform_lowercase:
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            break;
        case litehtml::text_transform_capitalize:
            if (!text.empty()) {
                bool capitalizeNext = true;
                for (auto& ch : text) {
                    if (std::isspace(static_cast<unsigned char>(ch))) {
                        capitalizeNext = true;
                    } else if (capitalizeNext) {
                        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        capitalizeNext = false;
                    }
                }
            }
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// CSS import
// ---------------------------------------------------------------------------

void BroContainer::import_css(litehtml::string& text, const litehtml::string& url,
                               litehtml::string& baseurl) {
    std::string base = baseurl.empty() ? baseUrl_ : baseurl;
    std::string path;
    if (!base.empty()) {
        path = base;
        if (path.back() != '/' && path.back() != '\\') {
            path += '/';
        }
        path += url;
    } else {
        path = url;
    }

    std::ifstream ifs(path);
    if (ifs.is_open()) {
        std::ostringstream ss;
        ss << ifs.rdbuf();
        text = ss.str();
    } else {
        LOG_WARN("import_css: failed to open '%s'", path.c_str());
    }
}

// ---------------------------------------------------------------------------
// Clipping
// ---------------------------------------------------------------------------

void BroContainer::set_clip(const litehtml::position& pos,
                             const litehtml::border_radiuses& bdr_radius) {
    clipStack_.push_back({pos, bdr_radius});
    renderer_->setClip(static_cast<float>(pos.x),
                       static_cast<float>(pos.y),
                       static_cast<float>(pos.width),
                       static_cast<float>(pos.height));
}

void BroContainer::del_clip() {
    if (!clipStack_.empty()) {
        clipStack_.pop_back();
    }
    renderer_->resetClip();

    // Restore previous clip if any.
    if (!clipStack_.empty()) {
        auto& top = clipStack_.back();
        renderer_->setClip(static_cast<float>(top.pos.x),
                           static_cast<float>(top.pos.y),
                           static_cast<float>(top.pos.width),
                           static_cast<float>(top.pos.height));
    }
}

// ---------------------------------------------------------------------------
// Viewport & media
// ---------------------------------------------------------------------------

void BroContainer::get_viewport(litehtml::position& viewport) const {
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = static_cast<litehtml::pixel_t>(viewportWidth_);
    viewport.height = static_cast<litehtml::pixel_t>(viewportHeight_);
}

litehtml::element::ptr BroContainer::create_element(
    const char* tag_name,
    const litehtml::string_map& /*attributes*/,
    const std::shared_ptr<litehtml::document>& doc) {
    // litehtml has specialized subclasses for certain HTML tags that
    // override behavior (e.g. el_style parses CSS, el_image loads images).
    // We return nullptr for those so litehtml creates the right subclass.
    // For everything else, we return BroElement which extends html_tag
    // with runtime CSS class selector support.
    // Only defer to litehtml for tags with non-trivial subclass behavior:
    //   style/script/link: parse CSS/JS, must not be html_tag
    //   img: image loading via compute_styles
    //   table/td/th/tr: table layout model (render_item_table)
    //   br: line break semantics
    //   base: sets document base URL
    // SVG elements get our custom replaced element
    if (strcmp(tag_name, "svg") == 0) {
        return std::make_shared<ElSvg>(doc, renderer_);
    }
    if (strcmp(tag_name, "input") == 0) {
        return std::make_shared<ElInput>(doc, renderer_);
    }
    if (strcmp(tag_name, "textarea") == 0) {
        return std::make_shared<ElTextarea>(doc, renderer_);
    }
    if (strcmp(tag_name, "select") == 0) {
        return std::make_shared<ElSelect>(doc, renderer_);
    }

    static const char* specialized_tags[] = {
        "style", "script", "link", "img", "table", "td", "th", "tr",
        "br", "base",
        nullptr
    };
    for (const char** t = specialized_tags; *t; ++t) {
        if (strcmp(tag_name, *t) == 0)
            return nullptr;
    }
    return std::make_shared<BroElement>(doc);
}

void BroContainer::get_media_features(litehtml::media_features& media) const {
    media.type = litehtml::media_type_screen;
    media.width = static_cast<litehtml::pixel_t>(viewportWidth_);
    media.height = static_cast<litehtml::pixel_t>(viewportHeight_);
    media.device_width = static_cast<litehtml::pixel_t>(viewportWidth_);
    media.device_height = static_cast<litehtml::pixel_t>(viewportHeight_);
    media.color = 8;
    media.color_index = 0;
    media.monochrome = 0;
    media.resolution = 96;
}

void BroContainer::get_language(litehtml::string& language, litehtml::string& culture) const {
    language = "en";
    culture = "";
}

} // namespace bro::layout
