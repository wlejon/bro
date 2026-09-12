#include "bronze_host/host_canvas2d.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "canvas/canvas2d.h"
#include "canvas/canvas_scene.h"
#include "dom/element.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace bro::bronze_host {

Value makeCanvas2DContextValue(Value canvasVal, dom::Element* el) {
    ObjectBuilder b;
    b.set("canvas", canvasVal);
    b.set("font", ev::fromUtf8("10px sans-serif"));
    b.set("textAlign", ev::fromUtf8("start"));
    b.set("textBaseline", ev::fromUtf8("alphabetic"));
    b.set("globalCompositeOperation", ev::fromUtf8("source-over"));

    b.accessor("fillStyle",
        [el](Value, std::span<const Value>) -> Value {
            if (el && el->canvasScene()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
                uint8_t r, g, b, a;
                cs->getFillColor(r, g, b, a);
                char buf[32];
                std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
                return ev::fromUtf8(buf);
            }
            return ev::fromUtf8("#000000");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
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
                char buf[32];
                std::snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
                return ev::fromUtf8(buf);
            }
            return ev::fromUtf8("#000000");
        },
        [el](Value, std::span<const Value> a) -> Value {
            if (el && el->canvasScene() && !a.empty()) {
                auto* cs = static_cast<canvas::CanvasScene*>(el->canvasScene());
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

    auto noop = [](Value, std::span<const Value>) -> Value { return ev::undefined(); };

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
    b.def("arc", 6, noop);
    b.def("arcTo", 5, noop);
    b.def("fill", 0, [el](Value, std::span<const Value>) -> Value {
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
    b.def("setTransform", 6, noop);
    b.def("resetTransform", 0, noop);
    b.def("clip", 0, noop);
    b.def("bezierCurveTo", 6, noop);
    b.def("quadraticCurveTo", 4, noop);
    b.def("drawImage", 9, noop);

    b.def("measureText", 1, [](Value, std::span<const Value> a) -> Value {
        std::string s = a.empty() ? "" : ev::toUtf8(a[0]);
        ObjectBuilder m;
        m.set("width", ev::fromDouble(static_cast<double>(s.size() * 10)));
        return m.get();
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
        ObjectBuilder img;
        img.set("width", ev::fromDouble(w));
        img.set("height", ev::fromDouble(h));
        Value arr = ev::createTypedArray(bronze::embed::elements::Uint8Clamped, static_cast<uint32_t>(pixels.size()));
        if (!pixels.empty()) {
            ev::fillTypedArray(arr, std::span<const uint8_t>(pixels.data(), pixels.size()));
        }
        img.set("data", arr);
        return img.get();
    });

    return b.get();
}

}  // namespace bro::bronze_host
