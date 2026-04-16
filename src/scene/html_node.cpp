#include "scene/html_node.h"
#include "dom/document.h"
#include "dom/element.h"
#include "render/skia_backend.h"
#include "layout/draw_traversal.h"
#include "layout/font_manager.h"
#include "layout/skia_text_metrics.h"
#include "util/log.h"

#include <include/core/SkSurface.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkImageInfo.h>

#include <algorithm>
#include <cmath>

namespace bro::scene {

// Minimal HTML shell used to bootstrap a standalone Document. Users mutate
// root_ directly (via innerHTML / appendChild / setTextContent); we keep a
// <body> around so layout has a sensible containing block.
static constexpr const char* kShellHtml =
    "<!DOCTYPE html><html><body><div id='__bro_html_node_root__'></div></body></html>";

// Minimal UA stylesheet — just enough for common HUD/label content. Scene
// library can't depend on engine::kDefaultStyles (engine depends on scene),
// so we keep a trimmed-down copy here. Users can override via inline <style>.
static constexpr const char* kShellUaCss = R"CSS(
html, body { display: block; margin: 0; padding: 0; }
div, p, section, article, header, footer, main, h1, h2, h3, h4, h5, h6, ul, ol, li, blockquote, table, tr, td, th { display: block; }
span, a, b, i, em, strong, small, code { display: inline; }
img, input, button, select, textarea { display: inline-block; }
head, title, meta, link, style, script { display: none; }
b, strong { font-weight: bold; }
i, em { font-style: italic; }
)CSS";

HtmlNode::HtmlNode(const std::string& name) : SceneNode(name) {
    doc_ = std::make_unique<dom::Document>();
    doc_->parse(kShellHtml, std::string(), kShellUaCss);
    root_ = doc_->getElementById("__bro_html_node_root__");
    if (!root_) {
        // Shouldn't happen; keep a non-null fallback so JS bindings can safely
        // return the root without null checks.
        root_ = doc_->body();
    }
}

HtmlNode::~HtmlNode() {
    releaseGL();
}

void HtmlNode::setHtml(const std::string& html) {
    if (!doc_ || !root_) return;
    doc_->parseInnerHTML(root_, html);
    doc_->markDirty();
    dirty_.store(true, std::memory_order_release);
}

void HtmlNode::setLayoutSize(float w, float h) {
    // Tiny layout sizes produce degenerate surfaces; clamp to at least 1 px.
    layoutW_ = std::max(1.0f, w);
    layoutH_ = std::max(1.0f, h);
    dirty_.store(true, std::memory_order_release);
}

void HtmlNode::setPxPerUnit(float p) {
    // Scale only — doesn't require a re-raster.
    pxPerUnit_ = (p > 0.0f) ? p : 100.0f;
}

void HtmlNode::materializePending(render::SkiaRenderer* renderer,
                                   layout::FontManager* fontMgr) {
    // JS mutations on the detached DOM set Document::dirty_ directly without
    // touching our atomic — pick that up here so imperative edits via
    // node.root trigger a re-raster.
    bool needsRender = dirty_.load(std::memory_order_acquire);
    if (!needsRender && doc_ && doc_->isDirty()) needsRender = true;
    if (!needsRender) return;
    if (!doc_ || !renderer || !fontMgr) return;

    int w = std::max(1, (int)std::ceil(layoutW_));
    int h = std::max(1, (int)std::ceil(layoutH_));

    // Layout on the raster thread using the raster renderer's font metrics.
    layout::SkiaTextMetrics metrics(renderer, fontMgr);
    doc_->resolveStyles();
    doc_->performLayout((float)w, (float)h, metrics);
    doc_->clearDirty();

    // CPU-backed SkSurface. We read pixels back to CPU and hand them to the
    // main thread's GL context; that keeps texture ownership on main thread
    // and side-steps Ganesh/GL fence synchronization entirely.
    SkImageInfo info = SkImageInfo::MakeN32Premul(w, h);
    sk_sp<SkSurface> cpuSurface = SkSurfaces::Raster(info);
    if (!cpuSurface) {
        LOG_ERROR("HtmlNode: failed to create CPU SkSurface (%dx%d)", w, h);
        return;
    }
    cpuSurface->getCanvas()->clear(SK_ColorTRANSPARENT);

    // Hand the raster renderer our surface for this draw pass, then restore.
    sk_sp<SkSurface> prev = renderer->switchSurface(cpuSurface);
    {
        layout::DrawTraversal dt(renderer, fontMgr);
        dom::Element* drawRoot = doc_->documentElement();
        if (drawRoot) dt.draw(drawRoot, 0.0f, 0.0f, w, h);
    }
    renderer->switchSurface(prev);

    // Read out premultiplied RGBA. The billboard fragment shader expects
    // premultiplied, so GL_RGBA + premul data needs no format conversion.
    const size_t rowBytes = (size_t)w * 4u;
    std::vector<uint8_t> buf((size_t)h * rowBytes);
    SkImageInfo readInfo = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType,
                                             kPremul_SkAlphaType);
    if (!cpuSurface->readPixels(readInfo, buf.data(), rowBytes, 0, 0)) {
        LOG_ERROR("HtmlNode: readPixels failed");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(stagingMutex_);
        pendingPixels_ = std::move(buf);
        pendingW_ = w;
        pendingH_ = h;
        pendingReady_ = true;
    }
    dirty_.store(false, std::memory_order_release);
}

void HtmlNode::uploadPendingTexture() {
    // Quick atomic-ish check before acquiring the mutex; pendingReady_ is
    // only written under the mutex but we tolerate a false-positive here.
    // Note: the mutex is always cheap when no work is pending (no blocking
    // on raster thread since materializePending only holds it briefly).
    std::unique_lock<std::mutex> lk(stagingMutex_);
    if (!pendingReady_) return;

    int w = pendingW_;
    int h = pendingH_;
    if (w <= 0 || h <= 0 || pendingPixels_.empty()) {
        pendingReady_ = false;
        return;
    }

    // Move pixels out so we release the mutex before the GL call; GL upload
    // is the expensive part and shouldn't block the raster thread's next
    // materialize.
    std::vector<uint8_t> pixels = std::move(pendingPixels_);
    pendingReady_ = false;
    lk.unlock();

    if (!texture_) glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    texW_ = w;
    texH_ = h;
}

void HtmlNode::releaseGL() {
    if (texture_) { glDeleteTextures(1, &texture_); texture_ = 0; }
    texW_ = 0;
    texH_ = 0;
}

} // namespace bro::scene
