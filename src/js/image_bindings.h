#pragma once

#include <qjsbind/qjsbind.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bro::util { class AssetMounts; }

namespace bro::js {

/// Decoded image pixel data (returned by ImageBindings::getImagePixels).
///
/// Two storage modes:
///   - Borrowed: `data` points into another object's buffer (e.g. an Image's
///     decoded `pixels` vector). `owned` is empty.
///   - Owned: `data` points into `owned`, which holds a freshly read pixel
///     buffer (e.g. snapshotted from an HTMLCanvasElement's surface). The
///     caller must keep this struct alive while using `data`.
///
/// Callers don't need to care which mode is in use — just read `data`,
/// `width`, `height`. The struct's destructor frees `owned` when it goes
/// out of scope.
struct ImagePixels {
    const uint8_t* data = nullptr;  // RGBA pixels
    int width = 0;
    int height = 0;
    std::vector<uint8_t> owned;     // optional; data may point into here

    /// Set when resolving this source ran Ganesh on the shared GL context —
    /// today, snapshotting a live `<canvas>`. Skia leaves the context in its
    /// own state (viewport sized to the canvas, its FBO/program bound) and
    /// documents that the caller must put things back, so a WebGL entry point
    /// that reads a canvas has to restoreState() before it draws again. Without
    /// it the *rest of the frame* renders into a canvas-sized corner: the first
    /// upload of a three.js CanvasTexture blanks everything drawn after it.
    bool disturbedGlState = false;
};

class ImageBindings {
public:
    /// Register the Image/HTMLImageElement constructor on the global object.
    /// basePath is the app directory used to resolve relative image src paths.
    /// mounts (when non-null) provides engine-supplied prefixes (`/lib`, ...).
    static void install(JSContext* ctx, const std::string& basePath,
                        const util::AssetMounts* mounts = nullptr);

    /// Forget a realm's asset base. Call when its JSContext goes away, so a
    /// reloaded or closed document does not leave an entry keyed on a freed
    /// context that a later context could be allocated on top of.
    static void cleanup(JSContext* ctx);

    /// Register only the bro.image function suite (decode/encode, geometric,
    /// color, preproc, ...) onto the brokit-built bro.image — no Image
    /// element class. For worker contexts, which have no DOM; relative path
    /// resolution reuses the base path set by the main-thread install().
    static void installKernels(JSContext* ctx);

    /// Try to extract ImagePixels from a JS value that is an Image object.
    /// Returns true if the value is a loaded Image with pixel data.
    static bool getImagePixels(JSValue val, ImagePixels& out);

    /// Create a new Image JS object (same as `new Image()` from JS).
    /// Used by createElement("img") to return Image objects.
    static JSValue createImage(JSContext* ctx);
};

} // namespace bro::js
