#pragma once

// Where an image URL's bytes come from, and the store request that decodes
// them (render/image_store.h).
//
// One resolver for every image consumer — an <img> src, a CSS url(), a
// border-image, `new Image()` — so a src names the same bytes, and therefore
// the same cached decode, whichever of them asked:
//   data:            the inline payload
//   blob:            the bytes registered when the URL was minted
//   http(s):         fetched on the decoder thread
//   C:\..., /mount   a file: drive-qualified as is, a leading slash through
//                    the engine's mounts (util::resolveAssetPath)
//   anything else    a file relative to `basePath` (the document's), else to
//                    the app directory
// A file's key carries its size and modification time, so a file rewritten on
// disk (a regenerated thumbnail) is a new image, not the stale decode.
//
// Resolving touches no file contents — a stat at most — so it is cheap on the
// page thread; the read and the decode happen in the request's work.

#include "render/image_store.h"

#include <memory>
#include <string>
#include <vector>

namespace bro::render {

struct ImageSource {
    enum class Kind : uint8_t { None, File, Bytes, Data, Remote };
    Kind kind = Kind::None;
    std::string key;    // the store key: these bytes, decoded this way
    std::string path;   // File: the filesystem path
    std::shared_ptr<const std::vector<uint8_t>> bytes;  // Bytes: a blob:'s registered bytes
    std::string url;    // Data: the data: URL (decoded with the image); Remote: the http(s) URL
    bool oriented = true;
    std::string error;  // None: why nothing can be read (a missing file, a revoked blob)
};

// Resolve `url`. `oriented` is whether the pixels are to be turned upright
// by their EXIF orientation (CSS image-orientation: from-image, the default).
ImageSource resolveImageSource(const std::string& url, const std::string& basePath,
                               bool oriented = true);

// The (cached, or newly started) request decoding `src`; null for Kind::None.
std::shared_ptr<ImageRequest> requestImage(const ImageSource& src);

// The natural size from the source's header, on the calling thread: a file's
// first 64 KB, or the inline bytes. False for a remote source (nothing read)
// and for bytes nothing recognises.
bool probeImageSource(const ImageSource& src, int& width, int& height, int& orientation,
                      bool& isSvg);

}  // namespace bro::render
