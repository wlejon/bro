#pragma once

#include "layout/font_manager.h"
#include "render/renderer.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::canvas { class CanvasScene; }
namespace bro::dom { class Element; class Node; }

namespace bro::layout {

// Image cache entry for background-image / <img> rendering
struct CachedImage {
    std::vector<uint8_t> data;
    int width = 0;
    int height = 0;
};

// Walks the DOM tree with layout boxes and issues Renderer draw calls.
class DrawTraversal {
public:
    DrawTraversal(render::Renderer* renderer, FontManager* fontManager);

    // Draw the full document tree
    void draw(dom::Element* root, float scrollX, float scrollY,
              int viewportW, int viewportH);

    // Draw a single element and its subtree (used for overlays)
    void drawElement(dom::Element* elem, float offsetX, float offsetY);

    // Load an image from disk into the cache
    void loadImage(const std::string& url, const std::string& basePath);

    // Set base path for resolving relative image URLs
    void setBasePath(const std::string& path) { basePath_ = path; }

    // Layer break callback: invoked when a canvas element is encountered
    // during traversal. The compositor uses this to split HTML rendering
    // into separate layers around canvas elements.
    using LayerBreakCallback = std::function<void(canvas::CanvasScene* scene,
                                                   float x, float y, float w, float h)>;
    void setLayerBreakCallback(LayerBreakCallback cb) { layerBreakCb_ = std::move(cb); }

    // Viewport
    void setViewport(int w, int h) { viewportW_ = w; viewportH_ = h; }

    // Access font manager (for replaced elements that need text measurement)
    FontManager* fontManager() { return fontManager_; }
    render::Renderer* renderer() { return renderer_; }

private:
    void drawNode(dom::Node* node, float offsetX, float offsetY);
    void drawElementContent(dom::Element* elem, float offsetX, float offsetY);
    void drawBackground(dom::Element* elem, float x, float y, float w, float h);
    void drawBorders(dom::Element* elem, float x, float y, float w, float h);
    void drawText(dom::Node* textNode, dom::Element* parent, float offsetX, float offsetY);

public:
    // Color parsing helper (public for shared use by element controls)
    static render::Color parseColor(const std::string& color);
    static bool tryParseColor(const std::string& color, render::Color& out);

private:

    // Font handle for an element's computed style
    uint64_t getFontHandle(dom::Element* elem);

    render::Renderer* renderer_;
    FontManager* fontManager_;
    std::string basePath_;
    int viewportW_ = 0;
    int viewportH_ = 0;

    std::unordered_map<std::string, CachedImage> imageCache_;
    LayerBreakCallback layerBreakCb_;
};

} // namespace bro::layout
