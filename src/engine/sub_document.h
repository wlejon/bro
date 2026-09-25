// Shared sub-document core — the build / teardown / record / replay / capture
// machinery behind every isolated document the engine hosts inside another one.

#pragma once

#include "engine/engine.h"
#include "engine/app_loader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::engine {

/// By-reference view of the members every hosted sub-document shares.
struct SubDocRef {
    std::vector<std::unique_ptr<canvas::CanvasScene>>& canvasScenes;
    std::unique_ptr<dom::Document>& document;
    dom::Element*& hoveredElement;
    int& boxW;
    int& boxH;
    render::CommandBuffer& cmdBuffer;
    render::SkiaRenderer::GPUSurface& surface;
    int& surfW;
    int& surfH;
    unsigned int& fboTexture;
};

/// A sub-app loaded off disk.
struct SubDocSource {
    bool ok = false;
    std::string appDir;       // directory the app was loaded from
    std::string resolvedSrc;  // src resolved against the parent's base path
    AppManifest manifest;
    std::string html;
    std::string authorStyles;
};

/// Resolve `srcAttr` against `basePath` (honouring asset mounts) and load the
/// bro app it names.
SubDocSource loadSubDocSource(const std::string& basePath, const std::string& srcAttr,
                              const util::AssetMounts* mounts, const char* what);

/// Parse the loaded HTML into a fresh Document whose media context is the sub-doc's own box.
void buildSubDocDocument(SubDocRef d, const SubDocSource& src,
                         const std::string& colorScheme, float resolution);

/// Execute scripts for a sub-document.
void runSubDocScripts(SubDocRef d, const SubDocSource& src, Engine* engine, bool isChild);

/// Replaced elements + the first style/layout pass at the box size.
void finishSubDocLoad(SubDocRef d, const SubDocSource& src,
                      render::Renderer* renderer, broaudio::Engine* audio,
                      layout::SkiaTextMetrics& metrics);

/// Log a clear warning for each <iframe> in a sub-document that cannot host one.
void warnNestedIframes(SubDocRef d, const char* what);

/// Report whether it needs (re)recording this frame — never recorded, DOM changed, or animating.
bool tickSubDoc(SubDocRef d, double nowMs);

/// Main thread: style + lay the sub-document out at its box, stage its canvas
/// commands, and record its paint into `cmdBuffer`.
void recordSubDoc(SubDocRef d, render::RecordingRenderer* rec,
                  layout::DrawTraversal* traversal, layout::SkiaTextMetrics& metrics);

/// Raster thread: replay `cmdBuffer` into a box-sized GPU surface.
void replaySubDoc(SubDocRef d, render::SkiaRenderer* renderer);

/// Synchronous main-thread capture: replay `cmdBuffer` into a throwaway surface.
std::vector<uint8_t> captureSubDoc(SubDocRef d, render::SkiaRenderer* skia,
                                   int& outW, int& outH);

/// Tear one sub-document down.
void teardownSubDoc(SubDocRef d);

} // namespace bro::engine
