// SkiaRenderer's GPU plumbing: the Ganesh GL context, the per-frame UI
// surface (Ganesh FBO or CPU raster fallback), offscreen GPU layer surfaces,
// and pixel readback for screenshots. Drawing lives in skia_backend.cpp.

#include "render/skia_backend.h"
#include "render/gl_context.h"

#include "broimage/encode.h"

#include <include/core/SkBitmap.h>
#include <include/core/SkColorSpace.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/gpu/ganesh/GrBackendSurface.h>
#include <include/gpu/ganesh/SkSurfaceGanesh.h>
#include <include/gpu/ganesh/gl/GrGLBackendSurface.h>
#include <include/gpu/ganesh/gl/GrGLDirectContext.h>
#include <include/gpu/ganesh/gl/GrGLInterface.h>
#if defined(__linux__) && !defined(__ANDROID__)
#include <cstring>
#include <dlfcn.h>
#include <include/gpu/ganesh/gl/GrGLAssembleInterface.h>
#include <SDL3/SDL_video.h>
namespace {
// Resolver used by GrGLMakeAssembledGLInterface on Linux. Tries SDL first
// (handles extension procs after a context is current), then dlsym for core
// GL entry points that some loaders won't return via getProcAddress.
GrGLFuncPtr linuxGLProc(void*, const char* name) {
    // Skia probes for eglQueryString / eglGetCurrentDisplay to harvest EGL
    // extensions. If we resolve those (libEGL.so happens to be in scope),
    // Skia then calls queryString(EGL_EXTENSIONS) and feeds the result into
    // its extension list — but the resulting strings are bogus when the
    // active context isn't actually owned by libEGL, and the subsequent
    // sort/strcmp segfaults. Mirror Skia's own GLX path: refuse EGL procs.
    if (strncmp(name, "egl", 3) == 0) return nullptr;

    // libglvnd's libGLdispatch routes core entry points through the active
    // context. Prefer dlsym so we get those dispatch stubs; SDL's vendor
    // procaddr can return mismatched function pointers.
    if (auto* p = dlsym(RTLD_DEFAULT, name)) {
        return reinterpret_cast<GrGLFuncPtr>(p);
    }
    if (auto* p = reinterpret_cast<void*>(SDL_GL_GetProcAddress(name))) {
        return reinterpret_cast<GrGLFuncPtr>(p);
    }
    return nullptr;
}
}  // namespace
#endif

namespace bro::render {

sk_sp<GrDirectContext> SkiaRenderer::createGrContext() {
    sk_sp<const GrGLInterface> glInterface;
#if defined(__linux__) && !defined(__ANDROID__)
    // Force libGL.so.1 into the global symbol namespace so dlsym(RTLD_DEFAULT)
    // resolves core GL entry points (glGetString, glGetIntegerv, etc.) for
    // the assembled fallback below — SDL_GL_GetProcAddress may return null
    // for these on some loaders.
    static void* glHandle = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    (void)glHandle;

    // Skia's GrGLMakeNativeInterface() on Linux uses GLX. Under WSL / Wayland
    // / EGL there's no current GLX context, so MakeGLX returns null.
    glInterface = GrGLMakeNativeInterface();
    if (!glInterface) {
        glInterface = GrGLMakeAssembledGLInterface(nullptr, &linuxGLProc);
        if (glInterface && !glInterface->validate()) glInterface.reset();
    }
#else
    glInterface = GrGLMakeNativeInterface();
#endif
    if (!glInterface) return nullptr;
    return GrDirectContexts::MakeGL(glInterface);
}

void SkiaRenderer::beginFrame(int width, int height) {
    if (!surface_ || surface_->width() != width || surface_->height() != height) {
        surface_.reset();

        if (uiTexture_) gl_->deleteTexture(uiTexture_);
        uiTexture_ = gl_->createTexture2D(width, height, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
        textureWidth_ = width;
        textureHeight_ = height;

        if (gpuMode_ && grContext_) {
            // Create FBO wrapping our texture for Skia GPU rendering
            if (gpuFBO_) glDeleteFramebuffers(1, &gpuFBO_);
            glGenFramebuffers(1, &gpuFBO_);
            glBindFramebuffer(GL_FRAMEBUFFER, gpuFBO_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, uiTexture_, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);

            // Wrap the FBO as a Skia GPU render target
            GrGLFramebufferInfo fbInfo;
            fbInfo.fFBOID = gpuFBO_;
            fbInfo.fFormat = GL_RGBA8;
            fbInfo.fProtected = skgpu::Protected::kNo;
            auto backendRT = GrBackendRenderTargets::MakeGL(
                width, height, 0, 0, fbInfo);
            surface_ = SkSurfaces::WrapBackendRenderTarget(
                grContext_.get(), backendRT,
                kTopLeft_GrSurfaceOrigin,
                kRGBA_8888_SkColorType,
                SkColorSpace::MakeSRGB(), nullptr);
        }

        if (!surface_) {
            // Fallback to CPU raster
            gpuMode_ = false;
            surface_ = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(width, height));
        }
    }

    if (gpuMode_ && grContext_) {
        // Reset Skia's GL state tracking (we share the context with Three.js)
        grContext_->resetContext();
    }

    canvas_ = surface_->getCanvas();
    enterCanvas();

    imageCache_.beginFrame();
}

void SkiaRenderer::endFrame() {
    if (canvas_) canvas_->restore();
    canvas_ = nullptr;

    if (gpuMode_ && grContext_) {
        // Flush all Skia GPU commands across all surfaces (including
        // HTML layer pool surfaces that were drawn to via switchSurface).
        grContext_->flushAndSubmit();
        pixelsPending_ = false;
    } else {
        pixelsPending_ = (surface_ && uiTexture_);
    }
}

void SkiaRenderer::uploadToGPU() {
    // GPU mode: already rendered to texture, nothing to upload
    if (!pixelsPending_ || !surface_ || !uiTexture_) return;
    pixelsPending_ = false;

    SkPixmap pixmap;
    if (!surface_->peekPixels(&pixmap)) return;

    gl_->uploadTexture2D(uiTexture_, pixmap.addr(),
                         static_cast<uint32_t>(pixmap.width()),
                         static_cast<uint32_t>(pixmap.height()),
                         GL_BGRA, GL_UNSIGNED_BYTE);
}

sk_sp<SkSurface> SkiaRenderer::switchSurface(sk_sp<SkSurface> newSurface) {
    // Restore the save() from beginFrame on the current surface
    if (canvas_) canvas_->restore();

    auto prev = surface_;
    surface_ = std::move(newSurface);
    canvas_ = surface_ ? surface_->getCanvas() : nullptr;
    if (canvas_) enterCanvas();

    return prev;
}

void SkiaRenderer::enterCanvas() {
    canvas_->restoreToCount(1);
    canvas_->resetMatrix();
    canvas_->clear(SK_ColorTRANSPARENT);
    if (deviceScale_ != 1.0f) canvas_->scale(deviceScale_, deviceScale_);
    canvas_->save();
}

GLuint SkiaRenderer::uploadSurfaceToTexture(SkSurface* surface, GLuint existingTex) {
    if (!surface || !gl_) return 0;

    SkPixmap pixmap;
    if (!surface->peekPixels(&pixmap)) return 0;

    int w = static_cast<int>(pixmap.width());
    int h = static_cast<int>(pixmap.height());

    GLuint tex = existingTex;
    if (!tex) {
        tex = gl_->createTexture2D(w, h, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);
    }
    gl_->uploadTexture2D(tex, pixmap.addr(), w, h, GL_BGRA, GL_UNSIGNED_BYTE);
    return tex;
}

SkiaRenderer::GPUSurface SkiaRenderer::createGPUSurface(int width, int height) {
    GPUSurface result;
    if (!gpuMode_ || !grContext_ || !gl_) return result;

    result.texture = gl_->createTexture2D(width, height, GL_RGBA8, GL_BGRA, GL_UNSIGNED_BYTE);

    glGenFramebuffers(1, &result.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, result.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, result.texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    GrGLFramebufferInfo fbInfo;
    fbInfo.fFBOID = result.fbo;
    fbInfo.fFormat = GL_RGBA8;
    fbInfo.fProtected = skgpu::Protected::kNo;
    auto backendRT = GrBackendRenderTargets::MakeGL(width, height, 0, 0, fbInfo);
    result.surface = SkSurfaces::WrapBackendRenderTarget(
        grContext_.get(), backendRT,
        kTopLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType,
        SkColorSpace::MakeSRGB(), nullptr);

    // The raw GL above (texture + FBO gen/bind) mutated GL state behind Ganesh's
    // back. When createGPUSurface runs mid-frame (e.g. an iframe or system-panel
    // surface allocated after the main HTML pass), Ganesh's cached GL state is
    // now stale — solid fills survive it, but glyph-atlas texture sampling reads
    // the wrong bindings and text silently paints nothing. Resync so any draw
    // into the new surface (text included) is correct regardless of when we ran.
    grContext_->resetContext();

    return result;
}

void SkiaRenderer::rewrapGPUSurface(GPUSurface& surf, int width, int height) {
    if (!gpuMode_ || !grContext_ || !surf.fbo) return;
    surf.surface.reset();
    GrGLFramebufferInfo fbInfo;
    fbInfo.fFBOID = surf.fbo;
    fbInfo.fFormat = GL_RGBA8;
    fbInfo.fProtected = skgpu::Protected::kNo;
    auto backendRT = GrBackendRenderTargets::MakeGL(width, height, 0, 0, fbInfo);
    surf.surface = SkSurfaces::WrapBackendRenderTarget(
        grContext_.get(), backendRT,
        kTopLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType,
        SkColorSpace::MakeSRGB(), nullptr);
}

void SkiaRenderer::destroyGPUSurface(GPUSurface& surf) {
    surf.surface.reset();
    if (surf.fbo) { glDeleteFramebuffers(1, &surf.fbo); surf.fbo = 0; }
    if (surf.texture && gl_) { gl_->deleteTexture(surf.texture); surf.texture = 0; }
}

bool SkiaRenderer::saveScreenshot(const std::string& path) {
    if (!surface_) return false;

    // For GPU mode, read pixels back from the GPU surface
    SkPixmap pixmap;
    sk_sp<SkImage> image;
    SkBitmap bitmap;

    if (gpuMode_) {
        image = surface_->makeImageSnapshot();
        if (!image) return false;
        auto info = SkImageInfo::MakeN32Premul(image->width(), image->height());
        bitmap.allocPixels(info);
        if (!image->readPixels(bitmap.pixmap(), 0, 0)) return false;
        pixmap = bitmap.pixmap();
    } else {
        if (!surface_->peekPixels(&pixmap)) return false;
    }

    int w = pixmap.width(), h = pixmap.height();

    // Convert from N32 (BGRA premultiplied on Windows) to RGBA for PNG
    std::vector<uint8_t> rgba(w * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(pixmap.addr32(0, y));
        uint8_t* dst = rgba.data() + y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2]; // R <- B
            dst[x * 4 + 1] = src[x * 4 + 1]; // G
            dst[x * 4 + 2] = src[x * 4 + 0]; // B <- R
            dst[x * 4 + 3] = src[x * 4 + 3]; // A
        }
    }

    return broimage::encode_png_file(path, rgba.data(), w, h, 4);
}

std::vector<uint8_t> SkiaRenderer::capturePixels() {
    if (!surface_) return {};

    SkPixmap pixmap;
    sk_sp<SkImage> image;
    SkBitmap bitmap;

    if (gpuMode_) {
        image = surface_->makeImageSnapshot();
        if (!image) return {};
        auto info = SkImageInfo::MakeN32Premul(image->width(), image->height());
        bitmap.allocPixels(info);
        if (!image->readPixels(bitmap.pixmap(), 0, 0)) return {};
        pixmap = bitmap.pixmap();
    } else {
        if (!surface_->peekPixels(&pixmap)) return {};
    }

    int w = pixmap.width(), h = pixmap.height();
    std::vector<uint8_t> rgba(w * h * 4);
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(pixmap.addr32(0, y));
        uint8_t* dst = rgba.data() + y * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = src[x * 4 + 2]; // R <- B
            dst[x * 4 + 1] = src[x * 4 + 1]; // G
            dst[x * 4 + 2] = src[x * 4 + 0]; // B <- R
            dst[x * 4 + 3] = src[x * 4 + 3]; // A
        }
    }
    return rgba;
}

} // namespace bro::render
