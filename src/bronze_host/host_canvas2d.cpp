#include "bronze_host/host_canvas2d.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include <cmath>
#include <string>

namespace bro::bronze_host {

Value makeCanvas2DContextValue(Value canvasVal) {
    ObjectBuilder b;
    b.set("canvas", canvasVal);
    b.set("fillStyle", ev::fromUtf8("#000000"));
    b.set("strokeStyle", ev::fromUtf8("#000000"));
    b.set("font", ev::fromUtf8("10px sans-serif"));
    b.set("textAlign", ev::fromUtf8("start"));
    b.set("textBaseline", ev::fromUtf8("alphabetic"));
    b.set("lineWidth", ev::fromDouble(1.0));
    b.set("globalAlpha", ev::fromDouble(1.0));
    b.set("globalCompositeOperation", ev::fromUtf8("source-over"));

    auto noop = [](Value, std::span<const Value>) -> Value { return ev::undefined(); };

    b.def("beginPath", 0, noop);
    b.def("closePath", 0, noop);
    b.def("arc", 6, noop);
    b.def("arcTo", 5, noop);
    b.def("fill", 0, noop);
    b.def("stroke", 0, noop);
    b.def("fillText", 4, noop);
    b.def("strokeText", 4, noop);
    b.def("fillRect", 4, noop);
    b.def("strokeRect", 4, noop);
    b.def("clearRect", 4, noop);
    b.def("moveTo", 2, noop);
    b.def("lineTo", 2, noop);
    b.def("rect", 4, noop);
    b.def("save", 0, noop);
    b.def("restore", 0, noop);
    b.def("translate", 2, noop);
    b.def("scale", 2, noop);
    b.def("rotate", 1, noop);
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

    b.def("getImageData", 4, [](Value, std::span<const Value> a) -> Value {
        uint32_t w = a.size() > 2 ? static_cast<uint32_t>(ev::toDouble(a[2])) : 1;
        uint32_t h = a.size() > 3 ? static_cast<uint32_t>(ev::toDouble(a[3])) : 1;
        uint32_t bytes = w * h * 4;
        ObjectBuilder img;
        img.set("width", ev::fromDouble(w));
        img.set("height", ev::fromDouble(h));
        img.set("data", ev::createTypedArray(bronze::embed::elements::Uint8Clamped, bytes));
        return img.get();
    });

    return b.get();
}

}  // namespace bro::bronze_host
