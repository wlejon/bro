#pragma once

#include <cstdint>
#include <string>

struct JSContext;
struct JSValue;

// Forward-declared as opaque — defined in QuickJS
typedef struct JSValue JSValue;

namespace bro::js {

/// Decoded image pixel data (returned by ImageBindings::getImageData).
struct ImagePixels {
    const uint8_t* data = nullptr;  // RGBA pixels
    int width = 0;
    int height = 0;
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
