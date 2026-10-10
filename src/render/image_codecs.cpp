#include "render/image_codecs.h"

#include "svg/svg_renderer.h"

#if BRO_WITH_WEBP
#include "render/webp_image.h"
#endif
#if BRO_WITH_AVIF
#include "render/avif_av1.h"
#endif

#include "broimage/codec.h"

#include <mutex>

namespace bro::render {

namespace {

// SVG as broimage sees it: rasterized at its intrinsic size (its
// width/height, else its viewBox). One without either has no pixel size and
// is not an image to broimage; the image store keeps such markup for the
// painter, which draws vectors at whatever size the box has.
bool svgSniff(const uint8_t* d, std::size_t n) {
    return svg::looksLikeSvg(reinterpret_cast<const char*>(d), n);
}

bool svgProbe(const uint8_t* d, std::size_t n, int& w, int& h, int& channels) {
    float fw = 0, fh = 0;
    svg::svgIntrinsicSize(reinterpret_cast<const char*>(d), n, fw, fh);
    if (fw < 1.0f || fh < 1.0f) return false;
    w = static_cast<int>(fw);
    h = static_cast<int>(fh);
    channels = 4;
    return true;
}

bool svgDecode(const uint8_t* d, std::size_t n, broimage::Image& out, std::string* why) {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!svg::rasterizeSvgMarkup(reinterpret_cast<const char*>(d), n, 0, 0, w, h, rgba) || rgba.empty()) {
        if (why) *why = "the SVG has no intrinsic size or did not parse";
        return false;
    }
    out.width = w;
    out.height = h;
    out.channels = 4;
    out.pixels = std::move(rgba);
    return true;
}

}  // namespace

void registerImageCodecs() {
    static std::once_flag once;
    std::call_once(once, [] {
#if BRO_WITH_WEBP
        registerWebPCodec();
#endif
#if BRO_WITH_AVIF
        // AVIF: broimage reads the HEIF container; dav1d decodes the AV1.
        // (HEIC needs nothing here: broimage hands it to the OS.)
        registerAvifDecoder();
#endif
        broimage::Codec svg;
        svg.name = "svg";
        svg.sniff = svgSniff;
        svg.probe = svgProbe;
        svg.decode = svgDecode;
        broimage::register_codec(svg);
    });
}

}  // namespace bro::render
