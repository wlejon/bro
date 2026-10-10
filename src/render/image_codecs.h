#pragma once

// The image formats bro decodes that broimage does not carry — WebP
// (libwebp from the Skia source bundle, render/webp_image.h) and SVG (Skia's
// SVG module, svg/svg_renderer.h) — registered with broimage
// (broimage/codec.h), so every broimage entry point reads them: `bro.image`
// (decodeOriented, probeDimensions, openFrames, ...) on the page and in
// workers, bro.thumb, the terminal's inline images, and the image store,
// which decodes through the same entries rather than a chain of its own.
//
// Once per process, from any thread; the engine calls it at startup and the
// image store before it decodes. Later calls are free.

namespace bro::render {

void registerImageCodecs();

}  // namespace bro::render
