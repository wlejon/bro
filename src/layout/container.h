#pragma once

#include "layout/font_manager.h"
#include <litehtml.h>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::render { class Renderer; }

namespace bro::layout {

class BroContainer : public litehtml::document_container {
public:
    BroContainer(render::Renderer* renderer, int viewportWidth, int viewportHeight);
    ~BroContainer() override = default;

    // ----- litehtml::document_container pure virtuals -----
    litehtml::uint_ptr create_font(const litehtml::font_description& descr,
                                   const litehtml::document* doc,
                                   litehtml::font_metrics* fm) override;
    void delete_font(litehtml::uint_ptr hFont) override;
    litehtml::pixel_t text_width(const char* text, litehtml::uint_ptr hFont) override;
    void draw_text(litehtml::uint_ptr hdc, const char* text, litehtml::uint_ptr hFont,
                   litehtml::web_color color, const litehtml::position& pos) override;
    litehtml::pixel_t pt_to_px(float pt) const override;
    litehtml::pixel_t get_default_font_size() const override;
    const char* get_default_font_name() const override;
    void draw_list_marker(litehtml::uint_ptr hdc, const litehtml::list_marker& marker) override;
    void load_image(const char* src, const char* baseurl, bool redraw_on_ready) override;
    void get_image_size(const char* src, const char* baseurl, litehtml::size& sz) override;
    void draw_image(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                    const std::string& url, const std::string& base_url) override;
    void draw_solid_fill(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                         const litehtml::web_color& color) override;
    void draw_linear_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::linear_gradient& gradient) override;
    void draw_radial_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                              const litehtml::background_layer::radial_gradient& gradient) override;
    void draw_conic_gradient(litehtml::uint_ptr hdc, const litehtml::background_layer& layer,
                             const litehtml::background_layer::conic_gradient& gradient) override;
    void draw_borders(litehtml::uint_ptr hdc, const litehtml::borders& borders,
                      const litehtml::position& draw_pos, bool root) override;
    void set_caption(const char* caption) override;
    void set_base_url(const char* base_url) override;
    void link(const std::shared_ptr<litehtml::document>& doc,
              const litehtml::element::ptr& el) override;
    void on_anchor_click(const char* url, const litehtml::element::ptr& el) override;
    void on_mouse_event(const litehtml::element::ptr& el, litehtml::mouse_event event) override;
    void set_cursor(const char* cursor) override;
    void transform_text(litehtml::string& text, litehtml::text_transform tt) override;
    void import_css(litehtml::string& text, const litehtml::string& url,
                    litehtml::string& baseurl) override;
    void set_clip(const litehtml::position& pos,
                  const litehtml::border_radiuses& bdr_radius) override;
    void del_clip() override;
    void get_viewport(litehtml::position& viewport) const override;
    litehtml::element::ptr create_element(const char* tag_name,
                                          const litehtml::string_map& attributes,
                                          const std::shared_ptr<litehtml::document>& doc) override;
    void get_media_features(litehtml::media_features& media) const override;
    void get_language(litehtml::string& language, litehtml::string& culture) const override;

    // Accessors
    void setViewport(int w, int h) { viewportWidth_ = w; viewportHeight_ = h; }
    const std::string& baseUrl() const { return baseUrl_; }
    const std::string& caption() const { return caption_; }

    // Draw call counters (reset each frame by engine)
    struct DrawStats {
        int fills = 0;
        int texts = 0;
        int borders = 0;
        void reset() { fills = texts = borders = 0; }
    };
    DrawStats drawStats;

private:
    render::Renderer* renderer_;
    FontManager fontManager_;
    int viewportWidth_;
    int viewportHeight_;
    std::string baseUrl_;
    std::string caption_;
    std::string cursor_;


    struct ClipEntry {
        litehtml::position pos;
        litehtml::border_radiuses radius;
    };
    std::vector<ClipEntry> clipStack_;

    // Image cache: URL -> raw file bytes (PNG/JPEG encoded)
    struct CachedImage {
        std::vector<uint8_t> data;
        int width = 0;
        int height = 0;
    };
    std::unordered_map<std::string, CachedImage> imageCache_;
};

} // namespace bro::layout
