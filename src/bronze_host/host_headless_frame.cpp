#include "bronze_host/host_headless_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_anchor_download.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_telemetry.h"
#include "engine/engine.h"
#include "engine/capture_path.h"
#include "canvas/canvas_scene.h"
#if BRO_WITH_3D
#include "scene/scene_renderer.h"
#endif
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/text_node.h"
#include "dom/node.h"
#include "dom/document.h"
#include "dom/shadow_root.h"
#include "broimage/encode.h"
#include "util/log.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

std::string fmtF(float v) {
    if (v == 0.0f) return "0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    std::string s(buf);
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

std::string fmtEdges(const htmlayout::layout::Edges& e) {
    if (e.top == e.right && e.right == e.bottom && e.bottom == e.left)
        return fmtF(e.top);
    if (e.top == e.bottom && e.left == e.right)
        return fmtF(e.top) + " " + fmtF(e.right);
    return fmtF(e.top) + " " + fmtF(e.right) + " " + fmtF(e.bottom) + " " + fmtF(e.left);
}

std::string elemDesc(bro::dom::Element* el) {
    std::string s = "<" + el->tagName();
    std::string id = el->id();
    if (!id.empty()) s += "#" + id;
    std::string cls = el->getAttribute("class");
    if (!cls.empty()) {
        std::istringstream iss(cls);
        std::string tok;
        while (iss >> tok) s += "." + tok;
    }
    s += ">";
    return s;
}

void absolutePos(bro::dom::Element* el, float& ax, float& ay) {
    auto& box = el->layoutBox();
    ax = box.contentRect.x - box.padding.left - box.border.left;
    ay = box.contentRect.y - box.padding.top - box.border.top;
    for (auto* lp = el->layoutParent(); lp; lp = lp->layoutParent()) {
        auto& pb = lp->layoutBox();
        ax += pb.contentRect.x;
        ay += pb.contentRect.y;
        ay -= lp->scrollTopValue();
    }
}

std::string buildInspectString(bro::dom::Element* el, bool verbose) {
    std::ostringstream out;
    auto& box = el->layoutBox();
    float ax, ay;
    absolutePos(el, ax, ay);

    out << elemDesc(el) << "\n";
    out << "  Box Model:\n";
    out << "    content:  " << fmtF(box.contentRect.width) << " x "
        << fmtF(box.contentRect.height) << "\n";
    out << "    padding:  " << fmtEdges(box.padding) << "\n";
    out << "    border:   " << fmtEdges(box.border) << "\n";
    out << "    margin:   " << fmtEdges(box.margin) << "\n";
    out << "    full:     " << fmtF(box.fullWidth()) << " x "
        << fmtF(box.fullHeight()) << "\n";

    out << "  Position:\n";
    out << "    relative: (" << fmtF(box.contentRect.x) << ", "
        << fmtF(box.contentRect.y) << ")\n";
    out << "    absolute: (" << fmtF(ax) << ", " << fmtF(ay) << ")\n";

    float scrollTop = el->scrollTopValue();
    float natH = box.naturalHeight;
    if (scrollTop > 0 || natH > box.contentRect.height + 0.5f) {
        out << "  Scroll:\n";
        out << "    scrollTop:    " << fmtF(scrollTop) << "\n";
        out << "    scrollHeight: " << fmtF(natH) << "\n";
        out << "    overflow:     " << fmtF(natH - box.contentRect.height) << "px hidden\n";
    }

    if (box.textTruncated)
        out << "  Flags: text-truncated\n";

    auto& styles = el->computedStyle();
    if (!styles.empty()) {
        out << "  Computed Styles:\n";
        if (verbose) {
            std::vector<std::pair<std::string, std::string>> sorted(styles.begin(), styles.end());
            std::sort(sorted.begin(), sorted.end());
            for (auto& [k, v] : sorted) {
                out << "    " << k << ": " << v << "\n";
            }
        } else {
            static const char* keys[] = {
                "display", "position", "flex-direction", "justify-content", "align-items",
                "width", "height", "min-width", "min-height", "max-width", "max-height",
                "overflow", "overflow-x", "overflow-y",
                "color", "background-color", "background",
                "font-size", "font-family", "font-weight",
                "opacity", "visibility", "z-index", "transform",
                "box-sizing", "text-align", "vertical-align",
                "white-space", "text-overflow",
                "gap", "row-gap", "column-gap", "flex-wrap", "flex-grow", "flex-shrink",
                "grid-template-columns", "grid-template-rows",
            };
            for (auto* key : keys) {
                auto it = styles.find(key);
                if (it != styles.end() && !it->second.empty()) {
                    out << "    " << key << ": " << it->second << "\n";
                }
            }
        }
    }

    auto& inlineStyle = el->style();
    if (!inlineStyle.empty()) {
        out << "  Inline Styles:\n";
        out << "    " << inlineStyle.cssText() << "\n";
    }

    auto& attrs = el->attributes();
    bool hasExtra = false;
    for (auto& [k, v] : attrs) {
        if (k == "id" || k == "class" || k == "style") continue;
        if (!hasExtra) { out << "  Attributes:\n"; hasExtra = true; }
        out << "    " << k << "=\"" << v << "\"\n";
    }

    int elemCount = 0, textCount = 0;
    for (auto* child : el->childNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) ++elemCount;
        else if (child->nodeType() == bro::dom::NodeType::Text) ++textCount;
    }
    out << "  Children: " << elemCount << " element" << (elemCount != 1 ? "s" : "")
        << ", " << textCount << " text node" << (textCount != 1 ? "s" : "") << "\n";

    if (el->hasShadow()) {
        auto* sr = el->shadowRoot();
        out << "  Shadow DOM: " << (sr->mode() == bro::dom::ShadowRoot::Mode::Open ? "open" : "closed") << "\n";
    }

    return out.str();
}

void buildTreeString(std::ostringstream& out, bro::dom::Element* el,
                     int depth, int maxDepth, const std::string& indent) {
    auto& box = el->layoutBox();
    float ax, ay;
    absolutePos(el, ax, ay);

    out << indent << elemDesc(el) << "  "
        << fmtF(box.fullWidth()) << "x" << fmtF(box.fullHeight())
        << " @ (" << fmtF(ax) << ", " << fmtF(ay) << ")";

    auto& styles = el->computedStyle();
    auto displayIt = styles.find("display");
    if (displayIt != styles.end() && displayIt->second != "block")
        out << " [" << displayIt->second << "]";
    auto posIt = styles.find("position");
    if (posIt != styles.end() && posIt->second != "static")
        out << " [" << posIt->second << "]";
    auto ovIt = styles.find("overflow");
    if (ovIt != styles.end() && ovIt->second != "visible")
        out << " [overflow:" << ovIt->second << "]";
    if (el->hasShadow())
        out << " [shadow]";

    out << "\n";

    if (depth >= maxDepth) return;

    std::string childIndent = indent + "  ";
    for (auto* child : el->composedChildNodes()) {
        if (child->nodeType() == bro::dom::NodeType::Element) {
            buildTreeString(out, static_cast<bro::dom::Element*>(child),
                           depth + 1, maxDepth, childIndent);
        } else if (child->nodeType() == bro::dom::NodeType::Text) {
            auto* text = static_cast<bro::dom::TextNode*>(child);
            std::string data = text->data();
            size_t start = data.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) continue;
            size_t end = data.find_last_not_of(" \t\n\r");
            data = data.substr(start, end - start + 1);
            if (data.size() > 40) data = data.substr(0, 37) + "...";
            out << childIndent << "#text \"" << data << "\"\n";
        }
    }
}

} // namespace

void installHeadlessFrame(engine::Engine& engine) {
    // getPixel(x, y)
    ev::registerGlobal("getPixel", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("getPixel(x, y) requires x and y");
            int x = satCast<int>(ev::toDouble(a[0]));
            int y = satCast<int>(ev::toDouble(a[1]));

            auto pixels = engine.capturePixels();
            int w = engine.contentWidth();
            int h = engine.contentHeight();
            int stride = engine.viewportWidth();

            ev::Persistent obj(ev::createObject());
            if (pixels.empty() || x < 0 || y < 0 || x >= w || y >= h) {
                obj.set(ev::setProperty(obj.get(), "r", ev::fromDouble(0)));
                obj.set(ev::setProperty(obj.get(), "g", ev::fromDouble(0)));
                obj.set(ev::setProperty(obj.get(), "b", ev::fromDouble(0)));
                obj.set(ev::setProperty(obj.get(), "a", ev::fromDouble(0)));
                return obj.get();
            }

            size_t offset = (static_cast<size_t>(y + engine.contentTop()) * stride
                             + (x + engine.contentLeft())) * 4;
            obj.set(ev::setProperty(obj.get(), "r", ev::fromDouble(pixels[offset])));
            obj.set(ev::setProperty(obj.get(), "g", ev::fromDouble(pixels[offset + 1])));
            obj.set(ev::setProperty(obj.get(), "b", ev::fromDouble(pixels[offset + 2])));
            obj.set(ev::setProperty(obj.get(), "a", ev::fromDouble(pixels[offset + 3])));
            return obj.get();
        }, 2, "getPixel"));

    // getFramePixel(x, y)
    ev::registerGlobal("getFramePixel", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("getFramePixel(x, y) requires x and y");
            int x = satCast<int>(ev::toDouble(a[0]));
            int y = satCast<int>(ev::toDouble(a[1]));

            auto pixels = engine.capturePixels();
            int w = engine.viewportWidth();
            int h = engine.viewportHeight();

            ev::Persistent obj(ev::createObject());
            if (pixels.empty() || x < 0 || y < 0 || x >= w || y >= h) {
                obj.set(ev::setProperty(obj.get(), "r", ev::fromDouble(0)));
                obj.set(ev::setProperty(obj.get(), "g", ev::fromDouble(0)));
                obj.set(ev::setProperty(obj.get(), "b", ev::fromDouble(0)));
                obj.set(ev::setProperty(obj.get(), "a", ev::fromDouble(0)));
                return obj.get();
            }

            size_t offset = (static_cast<size_t>(y) * w + x) * 4;
            obj.set(ev::setProperty(obj.get(), "r", ev::fromDouble(pixels[offset])));
            obj.set(ev::setProperty(obj.get(), "g", ev::fromDouble(pixels[offset + 1])));
            obj.set(ev::setProperty(obj.get(), "b", ev::fromDouble(pixels[offset + 2])));
            obj.set(ev::setProperty(obj.get(), "a", ev::fromDouble(pixels[offset + 3])));
            return obj.get();
        }, 2, "getFramePixel"));

    // screenshot(path [, selector])
    ev::registerGlobal("screenshot", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("screenshot() requires a path argument");
            std::string path = ev::toUtf8(a[0]);
            bool ok = false;
            if (a.size() >= 2 && ev::isString(a[1])) {
                std::string selector = ev::toUtf8(a[1]);
                auto* el = engine.querySelector(selector);
                if (!el) {
                    return ev::throwTypeError(std::string("screenshot: element not found: ") + selector);
                }
                bro::dom::AbsoluteRect r = bro::dom::absoluteBorderBox(el);
                float ax = r.x;
                float ay = r.y + static_cast<float>(engine.contentTop());
                int w = static_cast<int>(r.width);
                int h = static_cast<int>(r.height);
                ok = engine.screenshot(path, static_cast<int>(ax), static_cast<int>(ay), w, h);
            } else {
                ok = engine.screenshot(path);
            }
            if (!ok) {
                return ev::throwError(std::string("screenshot: could not write ") + path);
            }
            return ev::fromBool(true);
        }, 1, "screenshot"));

    // screenshotCanvas(path, selector)
    ev::registerGlobal("screenshotCanvas", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("screenshotCanvas(path, selector) requires both arguments");
            std::string path = ev::toUtf8(a[0]);
            std::string selector = ev::toUtf8(a[1]);
            auto* el = engine.querySelector(selector);
            if (!el) {
                return ev::throwTypeError(std::string("screenshotCanvas: element not found: ") + selector);
            }
            if (el->sceneGraph() || el->webglContext()) {
                return ev::throwTypeError(
                    std::string("screenshotCanvas: element has an active 3D scene or WebGL context, not a plain 2D canvas: ") +
                    selector + " — use screenshot(path, selector) instead");
            }
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            if (!cs) {
                return ev::throwTypeError(std::string("screenshotCanvas: element has no 2D canvas: ") + selector);
            }
            engine.flush();
            cs->flush();
            auto* surf = cs->surface();
            if (!surf) return ev::throwError("screenshotCanvas: no surface");
            int w = surf->width();
            int h = surf->height();
            if (w <= 0 || h <= 0) return ev::throwError("screenshotCanvas: zero-size canvas");
            auto pixels = cs->getImageData(0, 0, w, h);
            if (pixels.empty()) return ev::throwError("screenshotCanvas: read failed");

            bool ok = bro::ensureParentDir(path) &&
                      broimage::encode_png_file(path, pixels.data(), w, h, 4);
            if (!ok) {
                return ev::throwError(std::string("screenshotCanvas: could not write ") + path);
            }
            return ev::fromBool(true);
        }, 2, "screenshotCanvas"));

    // writeFile(path, data)
    ev::registerGlobal("writeFile", ev::makeFunction(
        [](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("writeFile(path, data) requires both arguments");
            std::string path = ev::toUtf8(a[0]);
            Value data = a[1];

            const uint8_t* bytes = nullptr;
            size_t len = 0;
            std::string text;
            if (auto info = ev::typedArrayInfo(data)) {
                bytes = info.data;
                len = info.byteLength;
            } else if (auto buf = ev::arrayBufferInfo(data)) {
                bytes = buf.data;
                len = buf.byteLength;
            } else if (ev::isObject(data)) {
                return ev::throwTypeError("writeFile: data must be a string, ArrayBuffer, or TypedArray");
            } else {
                text = ev::toUtf8(data);
                bytes = reinterpret_cast<const uint8_t*>(text.data());
                len = text.size();
            }

            std::error_code ec;
            std::filesystem::path p(path);
            if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

            std::ofstream out(p, std::ios::binary | std::ios::trunc);
            if (!out) {
                return ev::throwError(std::string("writeFile: could not open ") + path);
            }
            if (len) {
                out.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(len));
            }
            out.close();
            if (!out) {
                return ev::throwError(std::string("writeFile: could not write ") + path);
            }
            return ev::fromDouble(static_cast<double>(len));
        }, 2, "writeFile"));

    // lastDownload()
    ev::registerGlobal("lastDownload", ev::makeFunction(
        [](Value, std::span<const Value>) -> Value {
            const std::string& p = lastDownloadPath();
            return p.empty() ? ev::null() : ev::fromUtf8(p);
        }, 0, "lastDownload"));

    // inspect(selector [, verbose])
    ev::registerGlobal("inspect", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("inspect(selector [, verbose]) requires a selector");
            std::string selector = ev::toUtf8(a[0]);
            engine.flush();
            auto* el = engine.querySelector(selector);
            if (!el) {
                return ev::throwTypeError(std::string("inspect: no element matches '") + selector + "'");
            }
            bool verbose = a.size() >= 2 ? ev::toBool(a[1]) : false;
            std::string result = buildInspectString(el, verbose);
            return ev::fromUtf8(result);
        }, 1, "inspect"));

    // inspectTree(selector [, depth])
    ev::registerGlobal("inspectTree", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("inspectTree(selector [, depth]) requires a selector");
            std::string selector = ev::toUtf8(a[0]);
            engine.flush();
            auto* el = engine.querySelector(selector);
            if (!el) {
                return ev::throwTypeError(std::string("inspectTree: no element matches '") + selector + "'");
            }
            int maxDepth = a.size() >= 2 ? satCast<int>(ev::toDouble(a[1])) : 3;
            std::ostringstream out;
            buildTreeString(out, el, 0, maxDepth, "");
            return ev::fromUtf8(out.str());
        }, 1, "inspectTree"));

    // computedStyle(selector [, property])
    ev::registerGlobal("computedStyle", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("computedStyle(selector [, property]) requires a selector");
            std::string selector = ev::toUtf8(a[0]);
            engine.flush();
            auto* el = engine.querySelector(selector);
            if (!el) {
                return ev::throwTypeError(std::string("computedStyle: no element matches '") + selector + "'");
            }
            auto& styles = el->computedStyle();
            if (a.size() >= 2 && ev::isString(a[1])) {
                std::string prop = ev::toUtf8(a[1]);
                auto it = styles.find(prop);
                if (it != styles.end()) return ev::fromUtf8(it->second);
                return ev::fromUtf8("");
            }
            ev::Persistent obj(ev::createObject());
            for (auto& [k, v] : styles) {
                ev::Persistent val(ev::fromUtf8(v));
                obj.set(ev::setProperty(obj.get(), k, val.get()));
            }
            return obj.get();
        }, 1, "computedStyle"));

    // elements(selector)
    ev::registerGlobal("elements", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.empty()) return ev::throwTypeError("elements(selector) requires a selector");
            std::string selector = ev::toUtf8(a[0]);
            engine.flush();
            auto* doc = engine.document();
            if (!doc || !doc->documentElement()) {
                return ev::fromUtf8("(no document)\n");
            }
            auto results = doc->documentElement()->querySelectorAll(selector);
            std::ostringstream out;
            out << results.size() << " match" << (results.size() != 1 ? "es" : "") << ":\n";
            for (size_t i = 0; i < results.size(); ++i) {
                auto* el = results[i];
                auto& box = el->layoutBox();
                float ax, ay;
                absolutePos(el, ax, ay);
                out << "  [" << i << "] " << elemDesc(el) << "  "
                    << fmtF(box.fullWidth()) << "x" << fmtF(box.fullHeight())
                    << " @ (" << fmtF(ax) << ", " << fmtF(ay) << ")\n";
            }
            return ev::fromUtf8(out.str());
        }, 1, "elements"));

    // inspectOverlay(panelName, selector [, verbose])
    ev::registerGlobal("inspectOverlay", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("inspectOverlay(panelName, selector [, verbose])");
            std::string panel = ev::toUtf8(a[0]);
            std::string selector = ev::toUtf8(a[1]);
            auto* el = engine.overlayQuerySelector(panel, selector);
            if (!el) {
                return ev::throwTypeError(std::string("inspectOverlay: no element matches '") + selector +
                                          "' in panel '" + panel + "'");
            }
            bool verbose = a.size() >= 3 ? ev::toBool(a[2]) : false;
            std::string result = buildInspectString(el, verbose);
            return ev::fromUtf8(result);
        }, 2, "inspectOverlay"));

    // inspectOverlayTree(panelName, selector [, depth])
    ev::registerGlobal("inspectOverlayTree", ev::makeFunction(
        [&engine](Value, std::span<const Value> a) -> Value {
            if (a.size() < 2) return ev::throwTypeError("inspectOverlayTree(panelName, selector [, depth])");
            std::string panel = ev::toUtf8(a[0]);
            std::string selector = ev::toUtf8(a[1]);
            auto* el = engine.overlayQuerySelector(panel, selector);
            if (!el) {
                return ev::throwTypeError(std::string("inspectOverlayTree: no element matches '") + selector +
                                          "' in panel '" + panel + "'");
            }
            int maxDepth = a.size() >= 3 ? satCast<int>(ev::toDouble(a[2])) : 3;
            std::ostringstream out;
            buildTreeString(out, el, 0, maxDepth, "");
            return ev::fromUtf8(out.str());
        }, 2, "inspectOverlayTree"));

    // overlayPanels()
    ev::registerGlobal("overlayPanels", ev::makeFunction(
        [&engine](Value, std::span<const Value>) -> Value {
            auto names = engine.overlayPanelNames();
            ev::CallResult parsed = ev::parseJson("[]");
            ev::Persistent arr{parsed.value};
            for (uint32_t i = 0; i < names.size(); ++i) {
                ev::Persistent s{ev::fromUtf8(names[i])};
                arr.set(ev::setElement(arr.get(), i, s.get()));
            }
            return arr.get();
        }, 0, "overlayPanels"));

    // perf object
    ev::Persistent perf(ev::createObject());
    // The function is made (and rooted) in its own statement: in
    // setProperty(perf.get(), k, makeFunction(...)) the receiver may be read
    // before the allocation moves it.
    auto setPerfFn = [&perf](const char* name, uint32_t arity, ev::NativeFn fn) {
        ev::Persistent f(ev::makeFunction(std::move(fn), arity, name));
        perf.set(ev::setProperty(perf.get(), name, f.get()));
    };
    setPerfFn("now", 0,
        [](Value, std::span<const Value>) -> Value {
            auto now = std::chrono::steady_clock::now().time_since_epoch();
            double ms = std::chrono::duration<double, std::milli>(now).count();
            return ev::fromDouble(ms);
        });
    setPerfFn("reset", 0,
        [&engine](Value, std::span<const Value>) -> Value {
            if (engine.document()) engine.document()->resetPerf();
            return ev::undefined();
        });
    setPerfFn("stats", 0,
        [&engine](Value, std::span<const Value>) -> Value {
            if (!engine.document()) return ev::createObject();
            const auto& p = engine.document()->perf();
            ev::Persistent o(ev::createObject());
            auto num = [&](const char* k, double v) {
                o.set(ev::setProperty(o.get(), k, ev::fromDouble(v)));
            };
            num("styleMs", p.styleMs);
            num("cascadeMs", p.cascadeMs);
            num("styleDiffMs", p.styleDiffMs);
            num("styleManagersMs", p.styleManagersMs);
            num("genContentMs", p.genContentMs);
            num("pseudoResolves", static_cast<double>(p.pseudoResolves));
            num("buildMs", p.buildMs);
            num("invalidateMs", p.invalidateMs);
            num("layoutMs", p.layoutMs);
            num("layoutTreeMs", p.layoutTreeMs);
            num("layoutAbsMs", p.layoutAbsMs);
            num("layoutHitMs", p.layoutHitMs);
            num("syncMs", p.syncMs);
            num("totalMs", p.totalMs());
            num("passes", static_cast<double>(p.passes));
            num("treeRebuilds", static_cast<double>(p.treeRebuilds));
            num("elementsStyled", static_cast<double>(p.elementsStyled));
            num("nodesLaidOut", static_cast<double>(p.nodesLaidOut));
            num("nodeVisits", static_cast<double>(p.nodeVisits));
            num("nodeRevisitsSkipped", static_cast<double>(p.nodeRevisitsSkipped));
            num("nodesReused", static_cast<double>(p.nodesReused));
            num("measureCalls", static_cast<double>(p.measureCalls));
            num("styleLookups", static_cast<double>(p.styleLookups));
            num("reuseFailDirty", static_cast<double>(p.reuseFailDirty));
            num("reuseFailAvailW", static_cast<double>(p.reuseFailAvailW));
            num("reuseFailAvailH", static_cast<double>(p.reuseFailAvailH));
            num("reuseFailOverride", static_cast<double>(p.reuseFailOverride));
#if BRO_WITH_3D
            {
                scene::CullStats s = engine.sceneCullStats();
                ev::Persistent sc(ev::createObject());
                auto snum = [&](const char* k, int v) {
                    sc.set(ev::setProperty(sc.get(), k, ev::fromDouble(v)));
                };
                snum("meshDrawn", s.meshDrawn);
                snum("meshCulled", s.meshCulled);
                snum("instancedDrawn", s.instancedDrawn);
                snum("instancedCulled", s.instancedCulled);
                snum("splatDrawn", s.splatDrawn);
                snum("splatCulled", s.splatCulled);
                snum("particlesDrawn", s.particlesDrawn);
                snum("particlesCulled", s.particlesCulled);
                snum("billboardsDrawn", s.billboardsDrawn);
                snum("billboardsCulled", s.billboardsCulled);
                snum("shadowDrawn", s.shadowDrawn);
                snum("shadowCulled", s.shadowCulled);
                snum("shadowTilesTotal", s.shadowTilesTotal);
                snum("shadowTilesRendered", s.shadowTilesRendered);
                snum("shadowTilesCached", s.shadowTilesCached);
                o.set(ev::setProperty(o.get(), "scene", sc.get()));
            }
#endif
            {
                auto tel = getHostTelemetry();
                ev::Persistent bz(ev::createObject());
                auto bnum = [&](const char* k, double v) {
                    bz.set(ev::setProperty(bz.get(), k, ev::fromDouble(v)));
                };
                bnum("heapUsedBytes", static_cast<double>(tel.heapUsedBytes));
                bnum("heapCommittedBytes", static_cast<double>(tel.heapCommittedBytes));
                bnum("gcCollections", static_cast<double>(tel.gcCollections));
                bnum("gcPauseNs", static_cast<double>(tel.gcPauseNs));
                bnum("shapeTransitions", static_cast<double>(tel.shapeTransitions));
                o.set(ev::setProperty(o.get(), "bronze", bz.get()));
            }
            return o.get();
        });
    setPerfFn("gpuFrameMs", 0,
        [&engine](Value, std::span<const Value>) -> Value {
            return ev::fromDouble(engine.gpuFrameMs());
        });
    ev::registerGlobal("perf", perf.get());
}

} // namespace bro::bronze_host
