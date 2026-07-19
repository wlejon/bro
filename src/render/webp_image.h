#pragma once

// WebP decoding.
//
// WHY THIS EXISTS AT ALL, rather than falling out of a codec we already link:
// the pinned pre-built Skia was built without libwebp, so SkCodec rejects
// .webp on Windows, while a hand-built Linux/macOS Skia (whose build scripts
// set skia_use_libwebp_decode=true) accepts it. broimage's stb fallback can't
// cover the gap either — stb has no WebP. The result was a format that worked
// on one platform and failed on another with nothing but a warning line,
// which is worse than uniform absence. So bro decodes WebP itself, from
// libwebp compiled out of the Skia source bundle (third_party/skia/
// skia_modules.cmake) — no vcpkg, and ON in every profile including minimal.
//
// WHY ITS OWN TARGET: bro has two independent image entry points, and both
// have to agree or the split just moves rather than closes.
//   - src/js/image_bindings.cpp   `new Image()` / <img> — broimage::decode_file
//   - src/render/image_cache.cpp  the renderer's draw path — SkCodec, then
//                                 broimage::decode_memory
// Neither links the other, so this lives in a small library both depend on
// instead of being written twice.
//
// The natural home for all of this is broimage, which is the decode seam both
// paths already go through — but broimage vendors its dependencies (stb, four
// files) and has no access to bro's Skia bundle, so putting it there means
// vendoring libwebp into that repo. Worth doing if broimage standalone ever
// needs WebP; not required for bro to be consistent.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bro::render {

/// Decode WebP bytes to tightly-packed, UNPREMULTIPLIED RGBA (the same
/// convention broimage::decode_memory emits, so callers can treat the two
/// interchangeably).
///
/// Returns false when the bytes aren't WebP or the bitstream is unusable —
/// the two are not distinguished, because neither one is something a caller
/// can act on differently: both mean "try another decoder, or report a broken
/// image". `out` is left untouched on failure.
bool decodeWebP(const void* data, std::size_t len,
                int& width, int& height, std::vector<uint8_t>& out);

/// Read `path` and decode it as WebP. Convenience for the callers that hold a
/// path rather than bytes; same return contract, including for a file that
/// doesn't exist.
bool decodeWebPFile(const std::string& path,
                    int& width, int& height, std::vector<uint8_t>& out);

} // namespace bro::render
