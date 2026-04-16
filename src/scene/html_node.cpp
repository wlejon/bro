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
#include <vector>

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
    dirty_ = true;
}

void HtmlNode::setLayoutSize(float w, float h) {
    // Tiny layout sizes produce degenerate surfaces; clamp to at least 1 px.
    layoutW_ = std::max(1.0f, w);
    layoutH_ = std::max(1.0f, h);
    dirty_ = true;
}

void HtmlNode::setPxPerUnit(float p) {
    // Scale only — doesn't require a re-raster.
    pxPerUnit_ = (p > 0.0f) ? p : 100.0f;
}

void HtmlNode::materializePending(render::SkiaRenderer* renderer,
                                   layout::FontManager* fontMgr) {
    if (!doc_ || !renderer || !fontMgr) return;
    // Imperative JS edits via node.root bump Document::dirty_ directly
    // without touching ours — pick that up here.
    if (!dirty_ && !doc_->isDirty()) return;

    int w = std::max(1, (int)std::ceil(layoutW_));
    int h = std::max(1, (int)std::ceil(layoutH_));

    layout::SkiaTextMetrics metrics(renderer, fontMgr);
    doc_->resolveStyles();
    doc_->performLayout((float)w, (float)h, metrics);
    doc_->clearDirty();

    // CPU-backed SkSurface. We read pixels back and upload through GL rather
    // than using a Ganesh-backed surface, to keep this independent of which
    // GrDirectContext the caller's renderer owns.
    SkImageInfo info = SkImageInfo::MakeN32Premul(w, h);
    sk_sp<SkSurface> cpuSurface = SkSurfaces::Raster(info);
    if (!cpuSurface) {
        LOG_ERROR("HtmlNode: failed to create CPU SkSurface (%dx%d)", w, h);
        return;
    }
    cpuSurface->getCanvas()->clear(SK_ColorTRANSPARENT);

    // Hand the caller's renderer our surface for this draw pass, then restore.
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

    if (!texture_) glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    texW_ = w;
    texH_ = h;
    dirty_ = false;
}

void HtmlNode::releaseGL() {
    if (texture_) { glDeleteTextures(1, &texture_); texture_ = 0; }
    texW_ = 0;
    texH_ = 0;
}

} // namespace bro::scene
