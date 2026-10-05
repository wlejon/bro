#pragma once

#include "scene/scene_node.h"

#include "scene/texture_source.h"

#include <memory>
#include <string>

namespace bro::dom { class Document; class Element; }
namespace bro::render { class SkiaRenderer; }

namespace bro::scene {

/// A scene node that owns a detached dom::Document + Element subtree,
/// rasterizes it off-screen, and renders as a world-anchored billboard
/// (the scene's billboard pass).
///
/// Threading: the detached Document is main-thread-only. JS mutations,
/// style resolution, layout and paint all run on the main thread; the
/// renderer uploads the resulting pixels when their generation moves. HtmlNodes are small (HUD labels, HP bars) so single-threaded
/// materialization is cheap, and it sidesteps the need to synchronize the
/// detached Document with the raster thread.
class HtmlNode : public SceneNode {
public:
    explicit HtmlNode(const std::string& name = "");
    ~HtmlNode() override;   // out of line: dom::Document is incomplete here

    HtmlNode(const HtmlNode&) = delete;
    HtmlNode& operator=(const HtmlNode&) = delete;

    Type type() const override { return Type::Html; }

    /// Root element for imperative JS mutation. Never null after construction.
    dom::Element* root() const { return root_; }
    dom::Document* document() const { return doc_.get(); }

    /// Replace the root's children from an HTML string.
    void setHtml(const std::string& html);

    /// Layout size in CSS pixels. The raster surface is this size; the
    /// billboard in world space spans (w / pxPerUnit) × (h / pxPerUnit).
    void setLayoutSize(float w, float h);
    float layoutWidth() const { return layoutW_; }
    float layoutHeight() const { return layoutH_; }

    /// CSS pixels per world-unit. Bigger value = smaller billboard.
    void setPxPerUnit(float p);
    float pxPerUnit() const { return pxPerUnit_; }

    /// Mark the DOM subtree dirty so it re-rasterizes next frame.
    /// Typically called from JS binding glue after mutation.
    void markHtmlDirty() { dirty_ = true; }
    bool isHtmlDirty() const { return dirty_; }

    /// If dirty, resolve styles + layout + paint the subtree into texture().
    /// Must run on the main thread with a Skia renderer registered on it.
    void materializePending(render::SkiaRenderer* renderer);

    /// The last raster: premultiplied RGBA8, layout-size pixels.
    const NodeTexture& texture() const { return texture_; }

private:
    std::unique_ptr<dom::Document> doc_;
    dom::Element* root_ = nullptr;

    float layoutW_ = 200.0f;
    float layoutH_ = 50.0f;
    float pxPerUnit_ = 100.0f;

    bool dirty_ = true;

    NodeTexture texture_;
};

} // namespace bro::scene
