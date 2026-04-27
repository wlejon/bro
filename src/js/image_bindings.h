#pragma once

#include <qjsbind/qjsbind.h>

#include <cstdint>
#include <string>
#include <vector>

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
};

class ImageBindings {
public:
    /// Register the Image/HTMLImageElement constructor on the global object.
    /// basePath is the app directory used to resolve relative image src paths.
    static void install(JSContext* ctx, const std::string& basePath);

    /// Try to extract ImagePixels from a JS value that is an Image object.
    /// Returns true if the value is a loaded Image with pixel data.
    static bool getImagePixels(JSValue val, ImagePixels& out);

    /// Create a new Image JS object (same as `new Image()` from JS).
    /// Used by createElement("img") to return Image objects.
    static JSValue createImage(JSContext* ctx);
};

} // namespace bro::js
