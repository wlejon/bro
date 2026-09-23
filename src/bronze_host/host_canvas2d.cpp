#include "bronze_host/host_canvas2d.h"
#include "bronze_host/host_canvas2d_matrix.h"
#include "bronze_host/host_canvas_gradient.h"
#include "bronze_host/host_canvas_pattern.h"
#include "bronze_host/host_canvas_path2d.h"
#include "bronze_host/host_canvas2d_paths.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"
#include "canvas/canvas2d.h"
#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "layout/el_video.h"
#include "util/string_utils.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// HTML's serialization of a color, which is what the fillStyle / strokeStyle
// / shadowColor getters answer: `#rrggbb` when opaque, `rgba(r, g, b, a)`
// otherwise — so `ctx.fillStyle = 'red'; ctx.fillStyle` is "#ff0000", the
// string apps compare against in every browser.
std::string colorToRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return util::serializeCanvasColor(r, g, b, a);
}

// A gradient or pattern assigned to fillStyle/strokeStyle is answered back as
// the very object, so it is kept here (and saved/restored alongside the
// scene's state); a color is answered from the scene. Undefined in `fill` /
// `stroke` means the style is a color.
struct CustomStyles {
    ev::Persistent fill;
    ev::Persistent stroke;
    std::vector<ev::Persistent> fillStack;
    std::vector<ev::Persistent> strokeStack;

    void reset() {
        fill.set(ev::undefined());
        stroke.set(ev::undefined());
        fillStack.clear();
        strokeStack.clear();
    }
};

}  // namespace

Value makeCanvas2DContextValue(Value canvasVal, dom::Element* el) {
    // `canvasVal` is current only until the first allocation, and building the
    // context allocates on nearly every line: root it before anything else.
    ev::Persistent canvasRoot(canvasVal);

    auto tracker = std::make_shared<Canvas2DTransformTracker>();
    auto styles = std::make_shared<CustomStyles>();
    tracker->addSaveHook([styles]() {
        styles->fillStack.emplace_back(styles->fill.get());
        styles->strokeStack.emplace_back(styles->stroke.get());
    });
    tracker->addRestoreHook([styles]() {
        if (!styles->fillStack.empty()) {
            styles->fill.set(styles->fillStack.back().get());
            styles->fillStack.pop_back();
        }
        if (!styles->strokeStack.empty()) {
            styles->stroke.set(styles->strokeStack.back().get());
            styles->strokeStack.pop_back();
        }
    });
    // The scene resets its own drawing state; this is the binding's share of
    // it — the transform getTransform() answers, the save stack beside it,
    // and any gradient or pattern style object. The hook reaches the state
    // weakly: the context object's methods own it, the scene does not.
    if (el && el->canvasScene()) {
        std::weak_ptr<Canvas2DTransformTracker> weakTracker = tracker;
        std::weak_ptr<CustomStyles> weakStyles = styles;
        static_cast<canvas::CanvasScene*>(el->canvasScene())->setResetHook([weakTracker, weakStyles]() {
            if (auto t = weakTracker.lock()) t->reset();
            if (auto s = weakStyles.lock()) s->reset();
        });
    }

    ObjectBuilder b;
    b.set("canvas", canvasRoot.get());

    // fillStyle and strokeStyle share one setter body: a CanvasGradient or a
    // CanvasPattern is kept as the object, a string is parsed as a color, and
    // anything else (an unparseable string, a foreign object) is ignored as
    // the spec says, leaving the previous style in place.
    auto setStyle = [el, styles](bool fill, std::span<const Value> a) -> Value {
        ev::Persistent& custom = fill ? styles->fill : styles->stroke;
        if (!el || !el->canvasScene() || a.empty()) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        if (ev::isObject(a[0])) {
            if (auto* grad = hostCanvasGradientOf(a[0])) {
                if (fill) cs->setFillShader(grad->buildShader());
                else cs->setStrokeShader(grad->buildShader());
                custom.set(a[0]);
            } else if (auto* pat = hostCanvasPatternOf(a[0])) {
                if (fill) cs->setFillPattern(pat->data);
                else cs->setStrokePattern(pat->data);
                custom.set(a[0]);
            }
            return ev::undefined();
        }
        std::string str = ev::toUtf8(a[0]);
        uint8_t r, g, b, a_col;
        if (canvas::parseCSSColor(str, r, g, b, a_col)) {
            custom.set(ev::undefined());
            if (fill) cs->setFillColor(r, g, b, a_col);
            else cs->setStrokeColor(r, g, b, a_col);
        }
        return ev::undefined();
    };

    b.accessor("fillStyle",
        [el, styles](Value, std::span<const Value>) -> Value {
            if (!styles->fill.get().isUndefined()) {
                return styles->fill.get();
            }
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                uint8_t r, g, b, a;
                cs->getFillColor(r, g, b, a);
                return ev::fromUtf8(colorToRGBA(r, g, b, a));
            }
            return ev::fromUtf8("#000000");
        },
        [setStyle](Value, std::span<const Value> a) -> Value {
            return setStyle(true, a);
        });

    b.accessor("strokeStyle",
        [el, styles](Value, std::span<const Value>) -> Value {
            if (!styles->stroke.get().isUndefined()) {
                return styles->stroke.get();
            }
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                uint8_t r, g, b, a;
                cs->getStrokeColor(r, g, b, a);
                return ev::fromUtf8(colorToRGBA(r, g, b, a));
            }
            return ev::fromUtf8("#000000");
        },
        [setStyle](Value, std::span<const Value> a) -> Value {
            return setStyle(false, a);
        });

    b.accessor("lineWidth",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromDouble(cs->lineWidth());
            }
            return ev::fromDouble(1.0);
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setLineWidth(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("lineCap",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                int v = cs->lineCap();
                return ev::fromUtf8(v == 1 ? "round" : (v == 2 ? "square" : "butt"));
            }
            return ev::fromUtf8("butt");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                // An unknown string is ignored and the state keeps its value,
                // as the HTML spec says for lineCap and as the old binding did.
                std::string s = ev::toUtf8(a[0]);
                if (s == "butt") cs->setLineCap(0);
                else if (s == "round") cs->setLineCap(1);
                else if (s == "square") cs->setLineCap(2);
            }
            return ev::undefined();
        });

    b.accessor("lineJoin",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                int v = cs->lineJoin();
                return ev::fromUtf8(v == 1 ? "round" : (v == 2 ? "bevel" : "miter"));
            }
            return ev::fromUtf8("miter");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                std::string s = ev::toUtf8(a[0]);
                if (s == "miter") cs->setLineJoin(0);
                else if (s == "round") cs->setLineJoin(1);
                else if (s == "bevel") cs->setLineJoin(2);
            }
            return ev::undefined();
        });

    b.accessor("miterLimit",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromDouble(cs->miterLimit());
            }
            return ev::fromDouble(10.0);
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setMiterLimit(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("globalAlpha",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromDouble(cs->globalAlpha());
            }
            return ev::fromDouble(1.0);
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setGlobalAlpha(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("globalCompositeOperation",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                static const char* names[] = {
                    "source-over", "source-in", "source-out", "source-atop",
                    "destination-over", "destination-in", "destination-out", "destination-atop",
                    "lighten", "darken", "xor", "lighter",
                    "multiply", "screen", "overlay",
                    "color-dodge", "color-burn", "hard-light", "soft-light",
                    "difference", "exclusion"
                };
                int v = cs->globalCompositeOperation();
                return ev::fromUtf8((v >= 0 && v < 21) ? names[v] : "source-over");
            }
            return ev::fromUtf8("source-over");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                std::string s = ev::toUtf8(a[0]);
                static const char* names[] = {
                    "source-over", "source-in", "source-out", "source-atop",
                    "destination-over", "destination-in", "destination-out", "destination-atop",
                    "lighten", "darken", "xor", "lighter",
                    "multiply", "screen", "overlay",
                    "color-dodge", "color-burn", "hard-light", "soft-light",
                    "difference", "exclusion"
                };
                for (int i = 0; i < 21; ++i) {
                    if (s == names[i]) {
                        cs->setGlobalCompositeOperation(i);
                        break;
                    }
                }
            }
            return ev::undefined();
        });

    b.accessor("textAlign",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                static const char* names[] = {"start", "center", "right", "end", "left"};
                int v = cs->textAlign();
                return ev::fromUtf8((v >= 0 && v <= 4) ? names[v] : "start");
            }
            return ev::fromUtf8("start");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                std::string s = ev::toUtf8(a[0]);
                if (s == "start") cs->setTextAlign(0);
                else if (s == "center") cs->setTextAlign(1);
                else if (s == "right") cs->setTextAlign(2);
                else if (s == "end") cs->setTextAlign(3);
                else if (s == "left") cs->setTextAlign(4);
            }
            return ev::undefined();
        });

    b.accessor("textBaseline",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                static const char* names[] = {"alphabetic", "top", "middle", "bottom", "hanging", "ideographic"};
                int v = cs->textBaseline();
                return ev::fromUtf8((v >= 0 && v <= 5) ? names[v] : "alphabetic");
            }
            return ev::fromUtf8("alphabetic");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                std::string s = ev::toUtf8(a[0]);
                if (s == "alphabetic") cs->setTextBaseline(0);
                else if (s == "top") cs->setTextBaseline(1);
                else if (s == "middle") cs->setTextBaseline(2);
                else if (s == "bottom") cs->setTextBaseline(3);
                else if (s == "hanging") cs->setTextBaseline(4);
                else if (s == "ideographic") cs->setTextBaseline(5);
            }
            return ev::undefined();
        });

    b.accessor("direction",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                int v = cs->direction();
                return ev::fromUtf8(v == 1 ? "rtl" : (v == 2 ? "inherit" : "ltr"));
            }
            return ev::fromUtf8("ltr");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                std::string s = ev::toUtf8(a[0]);
                if (s == "ltr") cs->setDirection(0);
                else if (s == "rtl") cs->setDirection(1);
                else if (s == "inherit") cs->setDirection(2);
            }
            return ev::undefined();
        });

    b.accessor("font",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromUtf8(cs->fontString());
            }
            return ev::fromUtf8("10px sans-serif");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setFont(ev::toUtf8(a[0]));
            }
            return ev::undefined();
        });

    b.accessor("shadowBlur",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromDouble(cs->shadowBlur());
            }
            return ev::fromDouble(0.0);
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setShadowBlur(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("shadowOffsetX",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromDouble(cs->shadowOffsetX());
            }
            return ev::fromDouble(0.0);
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setShadowOffsetX(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("shadowOffsetY",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromDouble(cs->shadowOffsetY());
            }
            return ev::fromDouble(0.0);
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setShadowOffsetY(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("shadowColor",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                uint8_t r, g, b, a;
                cs->getShadowColor(r, g, b, a);
                return ev::fromUtf8(colorToRGBA(r, g, b, a));
            }
            return ev::fromUtf8("rgba(0, 0, 0, 0)");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                std::string s = ev::toUtf8(a[0]);
                uint8_t r, g, b, a;
                if (canvas::parseCSSColor(s, r, g, b, a)) {
                    cs->setShadowColor(r, g, b, a);
                }
            }
            return ev::undefined();
        });

    b.accessor("imageSmoothingEnabled",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromBool(cs->imageSmoothingEnabled());
            }
            return ev::fromBool(true);
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setImageSmoothingEnabled(ev::toBool(a[0]));
            }
            return ev::undefined();
        });

    b.accessor("imageSmoothingQuality",
        [el](Value, std::span<const Value>) -> Value {
            static const char* names[] = {"low", "medium", "high"};
            int q = 0;
            if (el && el->canvasScene()) {
                q = static_cast<canvas::CanvasScene*>(el->canvasScene())->imageSmoothingQuality();
            }
            return ev::fromUtf8(names[(q >= 0 && q <= 2) ? q : 0]);
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                // An enum attribute: a value outside the enum is ignored.
                std::string s = ev::toUtf8(a[0]);
                if (s == "low") cs->setImageSmoothingQuality(0);
                else if (s == "medium") cs->setImageSmoothingQuality(1);
                else if (s == "high") cs->setImageSmoothingQuality(2);
            }
            return ev::undefined();
        });

    b.accessor("filter",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                return ev::fromUtf8(static_cast<canvas::CanvasScene*>(el->canvasScene())->filterString());
            }
            return ev::fromUtf8("none");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                // An unparseable value is ignored and the filter kept.
                cs->setFilter(ev::toUtf8(a[0]));
            }
            return ev::undefined();
        });

    b.accessor("lineDashOffset",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromDouble(cs->lineDashOffset());
            }
            return ev::fromDouble(0.0);
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                cs->setLineDashOffset(static_cast<float>(ev::toDouble(a[0])));
            }
            return ev::undefined();
        });

    b.accessor("canvasWidth",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromDouble(cs->width());
            }
            return ev::fromDouble(0);
        }, nullptr);

    b.accessor("canvasHeight",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                return ev::fromDouble(cs->height());
            }
            return ev::fromDouble(0);
        }, nullptr);

    installCanvas2DPaths(b, el);

    b.def("fillText", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 3) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            float maxWidth = -1.0f;
            if (a.size() >= 4 && !ev::isUndefined(a[3]) && !ev::isNull(a[3])) {
                maxWidth = static_cast<float>(ev::toDouble(a[3]));
            }
            cs->fillText(ev::toUtf8(a[0]), static_cast<float>(ev::toDouble(a[1])), static_cast<float>(ev::toDouble(a[2])), maxWidth);
        }
        return ev::undefined();
    });

    b.def("strokeText", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 3) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            float maxWidth = -1.0f;
            if (a.size() >= 4 && !ev::isUndefined(a[3]) && !ev::isNull(a[3])) {
                maxWidth = static_cast<float>(ev::toDouble(a[3]));
            }
            cs->strokeText(ev::toUtf8(a[0]), static_cast<float>(ev::toDouble(a[1])), static_cast<float>(ev::toDouble(a[2])), maxWidth);
        }
        return ev::undefined();
    });

    b.def("measureText", 1, [el](Value, std::span<const Value> a) -> Value {
        std::string s = a.empty() ? "" : ev::toUtf8(a[0]);
        canvas::CanvasTextMetrics m;
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            m = cs->measureText(s);
        }
        ObjectBuilder obj;
        obj.set("width", ev::fromDouble(m.width));
        obj.set("actualBoundingBoxLeft", ev::fromDouble(m.actualLeft));
        obj.set("actualBoundingBoxRight", ev::fromDouble(m.actualRight));
        obj.set("actualBoundingBoxAscent", ev::fromDouble(m.actualAscent));
        obj.set("actualBoundingBoxDescent", ev::fromDouble(m.actualDescent));
        obj.set("fontBoundingBoxAscent", ev::fromDouble(m.fontAscent));
        obj.set("fontBoundingBoxDescent", ev::fromDouble(m.fontDescent));
        obj.set("emHeightAscent", ev::fromDouble(m.emAscent));
        obj.set("emHeightDescent", ev::fromDouble(m.emDescent));
        obj.set("hangingBaseline", ev::fromDouble(m.hangingBaseline));
        obj.set("alphabeticBaseline", ev::fromDouble(m.alphabeticBaseline));
        obj.set("ideographicBaseline", ev::fromDouble(m.ideographicBaseline));
        return obj.get();
    });



    b.def("setLineDash", 1, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.empty()) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        std::vector<float> segs;
        if (ev::isObject(a[0])) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(ev::getProperty(a[0], "length")));
            segs.reserve(len);
            for (uint32_t i = 0; i < len; ++i) {
                segs.push_back(static_cast<float>(ev::toDouble(ev::getElement(a[0], i))));
            }
        }
        // Stored as given: getLineDash() answers the list the caller set, and
        // the paint path (CanvasScene::applyStroke) is what doubles an odd
        // list, so doubling here too would make the getter answer a list four
        // times as long as the one set.
        cs->setLineDash(segs);
        return ev::undefined();
    });

    b.def("getLineDash", 0, [el](Value, std::span<const Value>) -> Value {
        if (!el || !el->canvasScene()) return hostArrayOf(0, [](size_t) { return ev::undefined(); });
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        const auto& d = cs->lineDash();
        return hostArrayOf(d.size(), [&d](size_t i) { return ev::fromDouble(d[i]); });
    });

    installCanvas2DTransformMethods(b, el, tracker);

    b.def("createLinearGradient", 4, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 4) return ev::undefined();
        float x0 = static_cast<float>(ev::toDouble(a[0]));
        float y0 = static_cast<float>(ev::toDouble(a[1]));
        float x1 = static_cast<float>(ev::toDouble(a[2]));
        float y1 = static_cast<float>(ev::toDouble(a[3]));
        return makeLinearGradientValue(x0, y0, x1, y1);
    });

    b.def("createRadialGradient", 6, [](Value, std::span<const Value> a) -> Value {
        if (a.size() < 6) return ev::undefined();
        float x0 = static_cast<float>(ev::toDouble(a[0]));
        float y0 = static_cast<float>(ev::toDouble(a[1]));
        float r0 = static_cast<float>(ev::toDouble(a[2]));
        float x1 = static_cast<float>(ev::toDouble(a[3]));
        float y1 = static_cast<float>(ev::toDouble(a[4]));
        float r1 = static_cast<float>(ev::toDouble(a[5]));
        return makeRadialGradientValue(x0, y0, r0, x1, y1, r1);
    });

    b.def("createPattern", 2, [](Value, std::span<const Value> a) -> Value {
        return createCanvasPatternValue(a);
    });

    b.def("drawImage", 9, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.empty()) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        Value src = a[0];

        const uint8_t* rgba = nullptr;
        int imgW = 0, imgH = 0;
        sk_sp<SkImage> skImg;

        if (auto* bmp = hostImageBitmapOfMut(src)) {
            if (bmp->closed) return ev::undefined();
            if (bmp->image) {
                skImg = bmp->image;
                imgW = bmp->width;
                imgH = bmp->height;
            } else if (!bmp->pixels.empty()) {
                rgba = bmp->pixels.data();
                imgW = bmp->width;
                imgH = bmp->height;
            }
        } else if (const HostImage* img = hostImageOf(src)) {
            if (img && img->ok && !img->rgba.empty()) {
                rgba = img->rgba.data();
                imgW = img->width;
                imgH = img->height;
            }
        } else if (dom::Element* srcEl = hostElementOf(src)) {
            if (auto* srcCs = static_cast<canvas::CanvasScene*>(srcEl->canvasScene())) {
                skImg = srcCs->snapshotImage();
                if (skImg) {
                    imgW = skImg->width();
                    imgH = skImg->height();
                }
            } else if (auto* vc = srcEl->videoControl()) {
                int vw = 0, vh = 0;
                const uint8_t* px = vc->currentFrameRgba(&vw, &vh);
                if (px && vw > 0 && vh > 0) {
                    rgba = px;
                    imgW = vw;
                    imgH = vh;
                }
            }
        }

        if (!rgba && !skImg) return ev::undefined();

        float sx = 0, sy = 0, sw = static_cast<float>(imgW), sh = static_cast<float>(imgH);
        float dx = 0, dy = 0, dw = static_cast<float>(imgW), dh = static_cast<float>(imgH);

        if (a.size() >= 9) {
            sx = static_cast<float>(ev::toDouble(a[1]));
            sy = static_cast<float>(ev::toDouble(a[2]));
            sw = static_cast<float>(ev::toDouble(a[3]));
            sh = static_cast<float>(ev::toDouble(a[4]));
            dx = static_cast<float>(ev::toDouble(a[5]));
            dy = static_cast<float>(ev::toDouble(a[6]));
            dw = static_cast<float>(ev::toDouble(a[7]));
            dh = static_cast<float>(ev::toDouble(a[8]));
        } else if (a.size() >= 5) {
            dx = static_cast<float>(ev::toDouble(a[1]));
            dy = static_cast<float>(ev::toDouble(a[2]));
            dw = static_cast<float>(ev::toDouble(a[3]));
            dh = static_cast<float>(ev::toDouble(a[4]));
        } else if (a.size() >= 3) {
            dx = static_cast<float>(ev::toDouble(a[1]));
            dy = static_cast<float>(ev::toDouble(a[2]));
        }

        if (skImg) {
            cs->drawImage(skImg, sx, sy, sw, sh, dx, dy, dw, dh);
        } else {
            cs->drawImage(rgba, imgW, imgH, sx, sy, sw, sh, dx, dy, dw, dh);
        }
        return ev::undefined();
    });

    b.def("getImageData", 4, [el](Value, std::span<const Value> a) -> Value {
        int x = a.size() > 0 ? static_cast<int>(ev::toDouble(a[0])) : 0;
        int y = a.size() > 1 ? static_cast<int>(ev::toDouble(a[1])) : 0;
        int w = a.size() > 2 ? static_cast<int>(ev::toDouble(a[2])) : 1;
        int h = a.size() > 3 ? static_cast<int>(ev::toDouble(a[3])) : 1;
        std::vector<uint8_t> pixels;
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            pixels = cs->getImageData(x, y, w, h);
        }
        if (pixels.empty()) {
            pixels.resize(static_cast<size_t>(w) * h * 4, 0);
        }
        return makeImageDataValue(w, h, pixels.data());
    });

    b.def("createImageData", 2, [](Value, std::span<const Value> a) -> Value {
        int w = 1, h = 1;
        if (a.size() >= 2) {
            w = static_cast<int>(ev::toDouble(a[0]));
            h = static_cast<int>(ev::toDouble(a[1]));
        } else if (a.size() == 1 && ev::isObject(a[0])) {
            w = static_cast<int>(ev::toDouble(ev::getProperty(a[0], "width")));
            h = static_cast<int>(ev::toDouble(ev::getProperty(a[0], "height")));
        }
        if (w <= 0) w = 1;
        if (h <= 0) h = 1;
        return makeImageDataValue(w, h, nullptr);
    });

    b.def("putImageData", 7, [el](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3 || !el || !el->canvasScene()) return ev::undefined();
        // Every read goes through a[0], a rooted argument slot: each
        // getProperty may allocate (or run a getter), which leaves a copied
        // Value naming the pre-collection address.
        int dx = static_cast<int>(ev::toDouble(a[1]));
        int dy = static_cast<int>(ev::toDouble(a[2]));
        const bool dirty = a.size() >= 7;
        int dirtyX = dirty ? static_cast<int>(ev::toDouble(a[3])) : 0;
        int dirtyY = dirty ? static_cast<int>(ev::toDouble(a[4])) : 0;
        int dirtyW = dirty ? static_cast<int>(ev::toDouble(a[5])) : 0;
        int dirtyH = dirty ? static_cast<int>(ev::toDouble(a[6])) : 0;
        int w = static_cast<int>(ev::toDouble(ev::getProperty(a[0], "width")));
        int h = static_cast<int>(ev::toDouble(ev::getProperty(a[0], "height")));
        Value dataVal = ev::getProperty(a[0], "data");
        // The byte pointer is heap-borrowed: nothing may allocate between
        // here and the putImageData that copies it out.
        auto info = ev::typedArrayInfo(dataVal);
        if (info.data && w > 0 && h > 0) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            if (dirty) {
                cs->putImageData(info.data, w, h, dx, dy, dirtyX, dirtyY, dirtyW, dirtyH);
            } else {
                cs->putImageData(info.data, w, h, dx, dy);
            }
        }
        return ev::undefined();
    });

    // Branded: `ctx instanceof CanvasRenderingContext2D` and
    // `ctx.constructor.name`, which a canvas library sniffs before deciding
    // it has a 2D context. The value handed back is the post-call address.
    // The prototype is fetched in its own statement: the class installs
    // lazily, and an allocation there would move the object after b.get()
    // had already been read as the other argument.
    Value proto = canvasRenderingContext2DHostClass().prototype();
    return ev::setPrototype(b.get(), proto);
}

}  // namespace bro::bronze_host
