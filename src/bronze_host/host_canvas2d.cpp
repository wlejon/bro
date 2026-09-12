#include "bronze_host/host_canvas2d.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "bronze_host/host_globals_internal.h"
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

    return b.get();
}

}  // namespace bro::bronze_host
