// The SVG element interfaces, as Chromium shapes them: SVGElement under
// Element, SVGGraphicsElement (getBBox / getCTM / getScreenCTM and
// `transform`, js/geometry.js) under it, SVGGeometryElement for the shapes,
// the text-content family, gradients, filter primitives and animation
// elements, and one interface per tag below those, so `path instanceof
// SVGPathElement`, `el.constructor.name` and Object.prototype.toString
// answer what a library sniffing the element kind expects. An SVG tag
// without its own interface (or an unknown one) is a plain SVGElement.

#include "bronze_host/host_html_interfaces.h"
#include "bronze_host/host_svg_interfaces.h"

#include <cctype>
#include <memory>
#include <string>
#include <unordered_map>

namespace bro::bronze_host {

namespace {

Value illegalSvgConstructor(Value, std::span<const Value>) {
    return ev::throwTypeError("Illegal constructor");
}

// An abstract interface: its name and the interface it extends ("" for
// SVGElement's direct children of Element, named here as "Element").
struct SvgBase {
    const char* name;
    const char* parent;
};
constexpr SvgBase kSvgBases[] = {
    {"SVGElement", "Element"},
    {"SVGGraphicsElement", "SVGElement"},
    {"SVGGeometryElement", "SVGGraphicsElement"},
    {"SVGTextContentElement", "SVGGraphicsElement"},
    {"SVGTextPositioningElement", "SVGTextContentElement"},
    {"SVGGradientElement", "SVGElement"},
    {"SVGAnimationElement", "SVGElement"},
    {"SVGComponentTransferFunctionElement", "SVGElement"},
};

// A per-tag interface: its name, its parent and the lowercase tag it wraps.
struct SvgTag {
    const char* name;
    const char* parent;
    const char* tag;
};
constexpr SvgTag kSvgTagInterfaces[] = {
    {"SVGSVGElement", "SVGGraphicsElement", "svg"},
    {"SVGGElement", "SVGGraphicsElement", "g"},
    {"SVGDefsElement", "SVGGraphicsElement", "defs"},
    {"SVGUseElement", "SVGGraphicsElement", "use"},
    {"SVGImageElement", "SVGGraphicsElement", "image"},
    {"SVGForeignObjectElement", "SVGGraphicsElement", "foreignobject"},
    {"SVGAElement", "SVGGraphicsElement", "a"},
    {"SVGSwitchElement", "SVGGraphicsElement", "switch"},
    {"SVGPathElement", "SVGGeometryElement", "path"},
    {"SVGRectElement", "SVGGeometryElement", "rect"},
    {"SVGCircleElement", "SVGGeometryElement", "circle"},
    {"SVGEllipseElement", "SVGGeometryElement", "ellipse"},
    {"SVGLineElement", "SVGGeometryElement", "line"},
    {"SVGPolylineElement", "SVGGeometryElement", "polyline"},
    {"SVGPolygonElement", "SVGGeometryElement", "polygon"},
    {"SVGTextElement", "SVGTextPositioningElement", "text"},
    {"SVGTSpanElement", "SVGTextPositioningElement", "tspan"},
    {"SVGTextPathElement", "SVGTextContentElement", "textpath"},
    {"SVGLinearGradientElement", "SVGGradientElement", "lineargradient"},
    {"SVGRadialGradientElement", "SVGGradientElement", "radialgradient"},
    {"SVGStopElement", "SVGElement", "stop"},
    {"SVGClipPathElement", "SVGElement", "clippath"},
    {"SVGMaskElement", "SVGElement", "mask"},
    {"SVGPatternElement", "SVGElement", "pattern"},
    {"SVGMarkerElement", "SVGElement", "marker"},
    {"SVGSymbolElement", "SVGElement", "symbol"},
    {"SVGFilterElement", "SVGElement", "filter"},
    {"SVGTitleElement", "SVGElement", "title"},
    {"SVGDescElement", "SVGElement", "desc"},
    {"SVGMetadataElement", "SVGElement", "metadata"},
    {"SVGStyleElement", "SVGElement", "style"},
    {"SVGScriptElement", "SVGElement", "script"},
    {"SVGViewElement", "SVGElement", "view"},
    {"SVGMPathElement", "SVGElement", "mpath"},
    {"SVGAnimateElement", "SVGAnimationElement", "animate"},
    {"SVGAnimateTransformElement", "SVGAnimationElement", "animatetransform"},
    {"SVGAnimateMotionElement", "SVGAnimationElement", "animatemotion"},
    {"SVGSetElement", "SVGAnimationElement", "set"},
    {"SVGFEBlendElement", "SVGElement", "feblend"},
    {"SVGFEColorMatrixElement", "SVGElement", "fecolormatrix"},
    {"SVGFEComponentTransferElement", "SVGElement", "fecomponenttransfer"},
    {"SVGFECompositeElement", "SVGElement", "fecomposite"},
    {"SVGFEConvolveMatrixElement", "SVGElement", "feconvolvematrix"},
    {"SVGFEDiffuseLightingElement", "SVGElement", "fediffuselighting"},
    {"SVGFEDisplacementMapElement", "SVGElement", "fedisplacementmap"},
    {"SVGFEDistantLightElement", "SVGElement", "fedistantlight"},
    {"SVGFEDropShadowElement", "SVGElement", "fedropshadow"},
    {"SVGFEFloodElement", "SVGElement", "feflood"},
    {"SVGFEFuncAElement", "SVGComponentTransferFunctionElement", "fefunca"},
    {"SVGFEFuncBElement", "SVGComponentTransferFunctionElement", "fefuncb"},
    {"SVGFEFuncGElement", "SVGComponentTransferFunctionElement", "fefuncg"},
    {"SVGFEFuncRElement", "SVGComponentTransferFunctionElement", "fefuncr"},
    {"SVGFEGaussianBlurElement", "SVGElement", "fegaussianblur"},
    {"SVGFEImageElement", "SVGElement", "feimage"},
    {"SVGFEMergeElement", "SVGElement", "femerge"},
    {"SVGFEMergeNodeElement", "SVGElement", "femergenode"},
    {"SVGFEMorphologyElement", "SVGElement", "femorphology"},
    {"SVGFEOffsetElement", "SVGElement", "feoffset"},
    {"SVGFEPointLightElement", "SVGElement", "fepointlight"},
    {"SVGFESpecularLightingElement", "SVGElement", "fespecularlighting"},
    {"SVGFESpotLightElement", "SVGElement", "fespotlight"},
    {"SVGFETileElement", "SVGElement", "fetile"},
    {"SVGFETurbulenceElement", "SVGElement", "feturbulence"},
};

constexpr size_t kBaseCount = sizeof(kSvgBases) / sizeof(kSvgBases[0]);
constexpr size_t kTagCount = sizeof(kSvgTagInterfaces) / sizeof(kSvgTagInterfaces[0]);

HostClass g_svgBaseClasses[kBaseCount];
HostClass g_svgTagClasses[kTagCount];
std::unordered_map<std::string, const HostClass*>* g_svgByName = nullptr;
std::unordered_map<std::string, const HostClass*>* g_svgByTag = nullptr;

}  // namespace

void installSvgInterfaces() {
    if (!g_svgByName) g_svgByName = new std::unordered_map<std::string, const HostClass*>();
    if (!g_svgByTag) g_svgByTag = new std::unordered_map<std::string, const HostClass*>();
    g_svgByName->clear();
    g_svgByTag->clear();
    (*g_svgByName)["Element"] = &elementHostClass();
    for (size_t i = 0; i < kBaseCount; ++i) {
        g_svgBaseClasses[i].install(kSvgBases[i].name, 0, illegalSvgConstructor, nullptr);
        g_svgBaseClasses[i].inherit(*(*g_svgByName)[kSvgBases[i].parent]);
        (*g_svgByName)[kSvgBases[i].name] = &g_svgBaseClasses[i];
    }
    for (size_t i = 0; i < kTagCount; ++i) {
        g_svgTagClasses[i].install(kSvgTagInterfaces[i].name, 0, illegalSvgConstructor, nullptr);
        g_svgTagClasses[i].inherit(*(*g_svgByName)[kSvgTagInterfaces[i].parent]);
        (*g_svgByTag)[kSvgTagInterfaces[i].tag] = &g_svgTagClasses[i];
    }
}

const HostClass& svgElementHostClass() { return g_svgBaseClasses[0]; }

Value svgInterfaceProto(const std::string& tagName) {
    std::string tag = tagName;
    for (char& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (g_svgByTag) {
        auto it = g_svgByTag->find(tag);
        if (it != g_svgByTag->end()) return it->second->prototype();
    }
    return g_svgBaseClasses[0].prototype();
}

}  // namespace bro::bronze_host
