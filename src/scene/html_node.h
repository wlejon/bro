#pragma once

#include "scene/scene_node.h"

#include <glad/gl.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bro::dom { class Document; class Element; }
namespace bro::render { class SkiaRenderer; }
namespace bro::layout { class FontManager; }

namespace bro::scene {

/// A scene node that owns a detached dom::Document + Element subtree,
/// rasterizes it off-screen, and renders as a world-anchored billboard via
/// the scene's mesh FBO pipeline.
///
/// Lifecycle:
///   - Constructor builds an empty dom::Document with a root <div>.
///   - setHtml() replaces the root's children (parsed by the document).
///   - JS code may grab node.root and mutate the subtree imperatively; each
///     mutation flips dirty_ = true.
///   - Raster thread calls materializePending() once per frame: if dirty,
///     re-resolve styles + layout + rasterize into an RGBA buffer.
///   - Main thread's render pass uploads pending pixels to the GL texture
///     and draws as a billboard.
class HtmlNode : public SceneNode {
public:
    explicit HtmlNode(const std::string& name = "");
    ~HtmlNode() override;

    HtmlNode(const HtmlNode&) = delete;
    HtmlNode& operator=(const HtmlNode&) = delete;

    Type type() const override { return Type::Html; }
    void onRender(SceneGraph& graph) override {}  // billboard path handles it

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
    void markHtmlDirty() { dirty_.store(true, std::memory_order_release); }
    bool isHtmlDirty() const { return dirty_.load(std::memory_order_acquire); }

    /// --- Raster-thread entry ---
    /// Re-rasterize if dirty (resolves styles, layouts, paints into a CPU
    /// RGBA8 buffer). Must run on the raster thread because Ganesh + font
    /// handles are thread-local.
    void materializePending(render::SkiaRenderer* renderer,
                            layout::FontManager* fontMgr);

    /// --- Main (render) thread ---
    /// Upload any pending RGBA buffer to the GL texture. Returns the
    /// texture id (0 if nothing has been rasterized yet).
    GLuint textureId() const { return texture_; }
    int textureWidth() const { return texW_; }
    int textureHeight() const { return texH_; }
    void uploadPendingTexture();

    void releaseGL();

private:
    std::unique_ptr<dom::Document> doc_;
    dom::Element* root_ = nullptr;

    float layoutW_ = 200.0f;
    float layoutH_ = 50.0f;
    float pxPerUnit_ = 100.0f;

    std::atomic<bool> dirty_{true};

    // Staged RGBA8 pixels produced on the raster thread, consumed on the
    // main/render thread. Guarded by stagingMutex_ because the two threads
    // overlap in time (raster runs while main composites).
    std::mutex stagingMutex_;
    std::vector<uint8_t> pendingPixels_;
    int pendingW_ = 0;
    int pendingH_ = 0;
    bool pendingReady_ = false;

    // Main-thread-owned GL texture. Created lazily in uploadPendingTexture.
    GLuint texture_ = 0;
    int texW_ = 0;
    int texH_ = 0;
};

} // namespace bro::scene
