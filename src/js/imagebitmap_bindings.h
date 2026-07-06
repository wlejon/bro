#pragma once

#include <include/core/SkImage.h>
#include <include/core/SkRefCnt.h>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

/// ImageBitmap — an immutable, pixel-constructed, drawable image.
///
/// The web-standard primitive that fills the gap between raw RGBA pixels and
/// something `drawImage` / `texImage2D` can consume. Backed by a raster
/// `SkImage` (no GrContext, no thread affinity), so a bitmap can be built in a
/// worker and drawn on the main thread, and Ganesh uploads it to a GPU texture
/// exactly once — repeated draws reuse that texture.
///
/// Created from JS via `createImageBitmap(source)` (returns a Promise); never
/// `new`-able. Transferable across the Worker boundary (see message_serializer).
class ImageBitmapBindings {
public:
    /// Register the ImageBitmap class + the global `createImageBitmap`.
    static void install(JSContext* ctx);

    /// Class ID — used by drawImage / the message serializer for type checks.
    static JSClassID classId();

    /// Wrap an existing SkImage into a fresh JS ImageBitmap on `ctx`.
    /// Used by createImageBitmap and by the postMessage deserializer.
    static JSValue wrap(JSContext* ctx, sk_sp<SkImage> img);

    /// Borrow the SkImage from a JS ImageBitmap. Returns nullptr if `val` is
    /// not an ImageBitmap or has been closed/neutered.
    static sk_sp<SkImage> getImage(JSValueConst val);

    /// Move the SkImage out of a JS ImageBitmap, leaving the source neutered
    /// (getImage returns nullptr afterward). Used by the postMessage serializer
    /// to transfer a bitmap across threads with no pixel copy.
    static sk_sp<SkImage> takeImage(JSValueConst val);

    /// Build a web-standard ImageData object: { width, height, data } with
    /// its prototype set to globalThis.ImageData.prototype, so
    /// `x instanceof ImageData` is true. `dataArr` (a Uint8ClampedArray) is
    /// consumed. Every producer of ImageData-shaped values (the ImageData
    /// constructor, ctx.getImageData, ctx.createImageData) must go through
    /// this — a plain JS_NewObject shape duck-types fine but fails
    /// instanceof, and a bare C-function constructor with no prototype made
    /// `instanceof ImageData` THROW (TypeError) rather than return false.
    static JSValue makeImageData(JSContext* ctx, int width, int height,
                                 JSValue dataArr);
};

} // namespace bro::js
