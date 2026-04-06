#include "svg/svg_renderer.h"
#include "dom/element.h"
#include "util/log.h"

#include <SkSVGDOM.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkFontMgr.h>
#include <include/core/SkStream.h>
#include <SkShaper_factory.h>

#ifdef _WIN32
#include <include/ports/SkTypeface_win.h>
#else
#include <include/ports/SkFontMgr_fontconfig.h>
#include <include/ports/SkFontScanner_FreeType.h>
#endif

namespace bro::svg {

static sk_sp<SkFontMgr> getFontMgr() {
    static sk_sp<SkFontMgr> mgr =
#ifdef _WIN32
        SkFontMgr_New_DirectWrite();
#else
        SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
    return mgr;
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
