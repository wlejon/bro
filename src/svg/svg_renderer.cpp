#include "svg/svg_renderer.h"
#include "dom/element.h"
#include "util/log.h"

#include <cstdlib>
#include <string>
#include <string_view>

#include <SkSVGDOM.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkStream.h>
#include <SkShaper_factory.h>

#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#elif defined(__APPLE__)
#include <include/ports/SkFontMgr_mac_ct.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif

namespace bro::svg {

static sk_sp<SkFontMgr> getFontMgr() {
    static sk_sp<SkFontMgr> mgr =
#ifdef _WIN32
        SkFontMgr_New_DirectWrite();
#elif defined(__APPLE__)
        SkFontMgr_New_CoreText(nullptr);
#else
        SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
    return mgr;
}

void renderSvgMarkup(render::Renderer* renderer,
                     const char* data, size_t len,
                     float x, float y, float w, float h) {
    if (!renderer || !data || len == 0) return;
    SkCanvas* canvas = renderer->getCanvas();
    if (!canvas) return;

    SkMemoryStream stream(data, len);
    auto dom = SkSVGDOM::Builder()
        .setFontManager(getFontMgr())
        .setTextShapingFactory(SkShapers::Primitive::Factory())
        .make(stream);
    if (!dom) return;

    // Determine SVG intrinsic size so we can scale to fit the requested rect
    // when the SVG declares an explicit width/height with no viewBox (Skia
    // would otherwise draw at intrinsic dimensions). Cheap re-parse of the
    // outer <svg ...> tag attributes; falls back to (w, h) if absent.
    float intrW = w, intrH = h;
    {
        std::string_view sv(data, len);
        auto svgPos = sv.find("<svg");
        if (svgPos != std::string_view::npos) {
            auto endPos = sv.find('>', svgPos);
            if (endPos != std::string_view::npos) {
                std::string tag(sv.substr(svgPos, endPos - svgPos));
                bool hasViewBox = tag.find("viewBox") != std::string::npos;
                auto attr = [&](const char* name) -> float {
                    std::string needle = std::string(" ") + name + "=";
                    auto p = tag.find(needle);
                    if (p == std::string::npos) return -1.0f;
                    p += needle.size();
                    if (p >= tag.size()) return -1.0f;
                    char q = tag[p];
                    if (q != '"' && q != '\'') return -1.0f;
                    ++p;
                    auto eq = tag.find(q, p);
                    if (eq == std::string::npos) return -1.0f;
                    return std::strtof(tag.substr(p, eq - p).c_str(), nullptr);
                };
                if (!hasViewBox) {
                    float aw = attr("width");
                    float ah = attr("height");
                    if (aw > 0) intrW = aw;
                    if (ah > 0) intrH = ah;
                }
            }
        }
    }

    dom->setContainerSize(SkSize::Make(intrW, intrH));
    canvas->save();
    canvas->translate(x, y);
    if (intrW > 0 && intrH > 0 && (intrW != w || intrH != h)) {
        canvas->scale(w / intrW, h / intrH);
    }
    dom->render(canvas);
    canvas->restore();
}

void renderSvg(render::Renderer* renderer, dom::Element* svgElement,
               float x, float y, float w, float h) {
    if (!renderer || !svgElement) return;

    SkCanvas* canvas = renderer->getCanvas();
    if (!canvas) return;

    std::string markup = svgElement->outerHTML();
    if (markup.empty()) return;

    SkMemoryStream stream(markup.data(), markup.size());
    auto dom = SkSVGDOM::Builder()
        .setFontManager(getFontMgr())
        .setTextShapingFactory(SkShapers::Primitive::Factory())
        .make(stream);
    if (!dom) {
        LOG_WARN("SVG: failed to parse <svg> element");
        return;
    }

    dom->setContainerSize(SkSize::Make(w, h));

    canvas->save();
    canvas->translate(x, y);
    dom->render(canvas);
    canvas->restore();
}

} // namespace bro::svg
