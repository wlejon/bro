#include "bronze_host/host_canvas2d.h"
#include "bronze_host/host_canvas_gradient.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
#include "bronze_host/host_html_interfaces.h"
#include "canvas/canvas2d.h"
#include "canvas/canvas_scene.h"
#include "dom/element.h"
#include "layout/el_video.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// The serialization the fillStyle / strokeStyle getters have always answered:
// `rgba(r,g,b,a)` with the alpha to two decimals (`1.00`, `0.50`), which is
// what apps that round-trip a style string compare against.
std::string colorToRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "rgba(%d,%d,%d,%.2f)", r, g, b, a / 255.0f);
    return buf;
}

}  // namespace

Value makeCanvas2DContextValue(Value canvasVal, dom::Element* el) {
    ObjectBuilder b;
    b.set("canvas", canvasVal);

    b.accessor("fillStyle",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                uint8_t r, g, b, a;
                cs->getFillColor(r, g, b, a);
                return ev::fromUtf8(colorToRGBA(r, g, b, a));
            }
            return ev::fromUtf8("rgba(0,0,0,1.00)");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                if (ev::isObject(a[0])) {
                    if (auto* grad = hostCanvasGradientOf(a[0])) {
                        cs->setFillShader(grad->buildShader());
                        return ev::undefined();
                    }
                }
                std::string str = ev::toUtf8(a[0]);
                uint8_t r, g, b, a_col;
                if (canvas::parseCSSColor(str, r, g, b, a_col)) {
                    cs->setFillColor(r, g, b, a_col);
                }
            }
            return ev::undefined();
        });

    b.accessor("strokeStyle",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                uint8_t r, g, b, a;
                cs->getStrokeColor(r, g, b, a);
                return ev::fromUtf8(colorToRGBA(r, g, b, a));
            }
            return ev::fromUtf8("rgba(0,0,0,1.00)");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                if (ev::isObject(a[0])) {
                    if (auto* grad = hostCanvasGradientOf(a[0])) {
                        cs->setStrokeShader(grad->buildShader());
                        return ev::undefined();
                    }
                }
                std::string str = ev::toUtf8(a[0]);
                uint8_t r, g, b, a_col;
                if (canvas::parseCSSColor(str, r, g, b, a_col)) {
                    cs->setStrokeColor(r, g, b, a_col);
                }
            }
            return ev::undefined();
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
                    "lighter", "darken", "xor", "lighter",
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
            return ev::fromUtf8("rgba(0,0,0,0)");
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

    b.def("beginPath", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->beginPath();
        }
        return ev::undefined();
    });

    b.def("closePath", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->closePath();
        }
        return ev::undefined();
    });

    b.def("fill", 1, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->fill();
        }
        return ev::undefined();
    });

    b.def("stroke", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->stroke();
        }
        return ev::undefined();
    });

    b.def("clip", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->clip();
        }
        return ev::undefined();
    });

    b.def("reset", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->reset();
        }
        return ev::undefined();
    });

    b.def("fillRect", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float w = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            float h = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
            cs->fillRect(x, y, w, h);
        }
        return ev::undefined();
    });

    b.def("strokeRect", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float w = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            float h = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
            cs->strokeRect(x, y, w, h);
        }
        return ev::undefined();
    });

    b.def("clearRect", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            float x = a.size() > 0 ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = a.size() > 1 ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float w = a.size() > 2 ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            float h = a.size() > 3 ? static_cast<float>(ev::toDouble(a[3])) : 0.0f;
            cs->clearRect(x, y, w, h);
        }
        return ev::undefined();
    });

    b.def("fillText", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 3) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->fillText(ev::toUtf8(a[0]), static_cast<float>(ev::toDouble(a[1])), static_cast<float>(ev::toDouble(a[2])));
        }
        return ev::undefined();
    });

    b.def("strokeText", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 3) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->strokeText(ev::toUtf8(a[0]), static_cast<float>(ev::toDouble(a[1])), static_cast<float>(ev::toDouble(a[2])));
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

    b.def("moveTo", 2, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 2) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->moveTo(static_cast<float>(ev::toDouble(a[0])), static_cast<float>(ev::toDouble(a[1])));
        }
        return ev::undefined();
    });

    b.def("lineTo", 2, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 2) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->lineTo(static_cast<float>(ev::toDouble(a[0])), static_cast<float>(ev::toDouble(a[1])));
        }
        return ev::undefined();
    });

    b.def("rect", 4, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 4) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->rect(static_cast<float>(ev::toDouble(a[0])), static_cast<float>(ev::toDouble(a[1])),
                     static_cast<float>(ev::toDouble(a[2])), static_cast<float>(ev::toDouble(a[3])));
        }
        return ev::undefined();
    });

    b.def("arc", 6, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 5) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        float cx = static_cast<float>(ev::toDouble(a[0]));
        float cy = static_cast<float>(ev::toDouble(a[1]));
        float r  = static_cast<float>(ev::toDouble(a[2]));
        float sa = static_cast<float>(ev::toDouble(a[3]));
        float ea = static_cast<float>(ev::toDouble(a[4]));
        bool acw = a.size() >= 6 ? ev::toBool(a[5]) : false;
        cs->arc(cx, cy, r, sa, ea, acw);
        return ev::undefined();
    });

    b.def("arcTo", 5, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 5) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        cs->arcTo(static_cast<float>(ev::toDouble(a[0])),
                  static_cast<float>(ev::toDouble(a[1])),
                  static_cast<float>(ev::toDouble(a[2])),
                  static_cast<float>(ev::toDouble(a[3])),
                  static_cast<float>(ev::toDouble(a[4])));
        return ev::undefined();
    });

    b.def("bezierCurveTo", 6, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 6) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        cs->bezierCurveTo(static_cast<float>(ev::toDouble(a[0])),
                          static_cast<float>(ev::toDouble(a[1])),
                          static_cast<float>(ev::toDouble(a[2])),
                          static_cast<float>(ev::toDouble(a[3])),
                          static_cast<float>(ev::toDouble(a[4])),
                          static_cast<float>(ev::toDouble(a[5])));
        return ev::undefined();
    });

    b.def("quadraticCurveTo", 4, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 4) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        cs->quadraticCurveTo(static_cast<float>(ev::toDouble(a[0])),
                             static_cast<float>(ev::toDouble(a[1])),
                             static_cast<float>(ev::toDouble(a[2])),
                             static_cast<float>(ev::toDouble(a[3])));
        return ev::undefined();
    });

    b.def("ellipse", 8, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 7) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        float cx  = static_cast<float>(ev::toDouble(a[0]));
        float cy  = static_cast<float>(ev::toDouble(a[1]));
        float rx  = static_cast<float>(ev::toDouble(a[2]));
        float ry  = static_cast<float>(ev::toDouble(a[3]));
        float rot = static_cast<float>(ev::toDouble(a[4]));
        float sa  = static_cast<float>(ev::toDouble(a[5]));
        float ea  = static_cast<float>(ev::toDouble(a[6]));
        bool acw  = a.size() >= 8 ? ev::toBool(a[7]) : false;
        cs->ellipse(cx, cy, rx, ry, rot, sa, ea, acw);
        return ev::undefined();
    });

    b.def("isPointInPath", 2, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 2) return ev::fromBool(false);
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        bool in = cs->isPointInPath(static_cast<float>(ev::toDouble(a[0])),
                                   static_cast<float>(ev::toDouble(a[1])));
        return ev::fromBool(in);
    });

    b.def("polyline", 1, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.empty()) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        Value arg = a[0];
        auto tinfo = ev::typedArrayInfo(arg);
        if (tinfo.data && tinfo.byteLength >= sizeof(float) * 2) {
            int numPoints = static_cast<int>(tinfo.byteLength / (sizeof(float) * 2));
            cs->polyline(reinterpret_cast<const float*>(tinfo.data), numPoints);
            return ev::undefined();
        }
        if (ev::isObject(arg)) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(ev::getProperty(arg, "length")));
            if (len >= 2) {
                std::vector<float> pts;
                pts.reserve(len);
                for (uint32_t i = 0; i < len; ++i) {
                    pts.push_back(static_cast<float>(ev::toDouble(ev::getElement(arg, i))));
                }
                cs->polyline(pts.data(), static_cast<int>(pts.size() / 2));
            }
        }
        return ev::undefined();
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

    b.def("save", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->save();
        }
        return ev::undefined();
    });

    b.def("restore", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->restore();
        }
        return ev::undefined();
    });

    b.def("translate", 2, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 2) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->translate(static_cast<float>(ev::toDouble(a[0])), static_cast<float>(ev::toDouble(a[1])));
        }
        return ev::undefined();
    });

    b.def("scale", 2, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 2) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->scale(static_cast<float>(ev::toDouble(a[0])), static_cast<float>(ev::toDouble(a[1])));
        }
        return ev::undefined();
    });

    b.def("rotate", 1, [el](Value, std::span<const Value> a) -> Value {
        if (el && el->canvasScene() && a.size() >= 1) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->rotate(static_cast<float>(ev::toDouble(a[0])));
        }
        return ev::undefined();
    });

    b.def("setTransform", 6, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene()) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        if (a.size() >= 6) {
            cs->setTransform(static_cast<float>(ev::toDouble(a[0])),
                             static_cast<float>(ev::toDouble(a[1])),
                             static_cast<float>(ev::toDouble(a[2])),
                             static_cast<float>(ev::toDouble(a[3])),
                             static_cast<float>(ev::toDouble(a[4])),
                             static_cast<float>(ev::toDouble(a[5])));
        } else {
            cs->resetTransform();
        }
        return ev::undefined();
    });

    b.def("resetTransform", 0, [el](Value, std::span<const Value>) -> Value {
        if (el && el->canvasScene()) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->resetTransform();
        }
        return ev::undefined();
    });

    b.def("transform", 6, [el](Value, std::span<const Value> a) -> Value {
        if (!el || !el->canvasScene() || a.size() < 6) return ev::undefined();
        auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
        cs->transform(static_cast<float>(ev::toDouble(a[0])),
                      static_cast<float>(ev::toDouble(a[1])),
                      static_cast<float>(ev::toDouble(a[2])),
                      static_cast<float>(ev::toDouble(a[3])),
                      static_cast<float>(ev::toDouble(a[4])),
                      static_cast<float>(ev::toDouble(a[5])));
        return ev::undefined();
    });

    b.def("getTransform", 0, [](Value, std::span<const Value>) -> Value {
        ObjectBuilder m;
        m.set("a", ev::fromDouble(1.0));
        m.set("b", ev::fromDouble(0.0));
        m.set("c", ev::fromDouble(0.0));
        m.set("d", ev::fromDouble(1.0));
        m.set("e", ev::fromDouble(0.0));
        m.set("f", ev::fromDouble(0.0));
        return m.get();
    });

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

    b.def("putImageData", 3, [el](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3 || !el || !el->canvasScene()) return ev::undefined();
        Value imgData = a[0];
        int dx = static_cast<int>(ev::toDouble(a[1]));
        int dy = static_cast<int>(ev::toDouble(a[2]));
        int w = static_cast<int>(ev::toDouble(ev::getProperty(imgData, "width")));
        int h = static_cast<int>(ev::toDouble(ev::getProperty(imgData, "height")));
        Value dataVal = ev::getProperty(imgData, "data");
        auto info = ev::typedArrayInfo(dataVal);
        if (info.data && w > 0 && h > 0) {
            auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
            cs->putImageData(info.data, w, h, dx, dy);
        }
        return ev::undefined();
    });

    // Branded: `ctx instanceof CanvasRenderingContext2D` and
    // `ctx.constructor.name`, which a canvas library sniffs before deciding
    // it has a 2D context. The value handed back is the post-call address.
    return ev::setPrototype(b.get(), canvasRenderingContext2DHostClass().prototype());
}

}  // namespace bro::bronze_host
