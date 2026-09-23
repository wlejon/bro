// Transform feedback bindings for WebGL2.
// Wraps webgl::WebGL2RenderingContext transform feedback methods.

#include "bronze_host/gl_internal.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

void installGlTransformFeedback(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    b.def("createTransformFeedback", 0, [c](Value, std::span<const Value>) {
        return wrapTransformFeedback(live(c)->createTransformFeedback());
    });
    b.def("deleteTransformFeedback", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteTransformFeedback(transformFeedbackOf(argAt(a, 0)));
        return ev::undefined();
    });
    b.def("bindTransformFeedback", 2, [c](Value, std::span<const Value> a) {
        live(c)->bindTransformFeedback(u32At(a, 0), transformFeedbackOf(argAt(a, 1)));
        return ev::undefined();
    });
    b.def("beginTransformFeedback", 1, [c](Value, std::span<const Value> a) {
        live(c)->beginTransformFeedback(u32At(a, 0));
        return ev::undefined();
    });
    b.def("endTransformFeedback", 0, [c](Value, std::span<const Value> a) {
        live(c)->endTransformFeedback();
        return ev::undefined();
    });
    b.def("pauseTransformFeedback", 0, [c](Value, std::span<const Value> a) {
        live(c)->pauseTransformFeedback();
        return ev::undefined();
    });
    b.def("resumeTransformFeedback", 0, [c](Value, std::span<const Value> a) {
        live(c)->resumeTransformFeedback();
        return ev::undefined();
    });
    b.def("transformFeedbackVaryings", 3, [c](Value, std::span<const Value> a) {
        auto prog = webgl::WebGLProgram{idOf(argAt(a, 0), GlCell::Program)};
        Value namesVal = argAt(a, 1);
        std::vector<std::string> names;
        if (ev::isObject(namesVal)) {
            ev::Persistent root(namesVal);
            Value lenV = ev::getProperty(root.get(), "length");
            uint32_t n = 0;
            if (!ev::isUndefined(lenV) && !ev::isObject(lenV) &&
                lengthWithin(ev::toDouble(lenV), kMaxHostListLength, n)) {
                names.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    names.push_back(ev::toUtf8(ev::getElement(root.get(), i)));
                }
            }
        }
        live(c)->transformFeedbackVaryings(prog, names, u32At(a, 2));
        return ev::undefined();
    });
    b.def("getTransformFeedbackVarying", 2, [c](Value, std::span<const Value> a) {
        auto prog = webgl::WebGLProgram{idOf(argAt(a, 0), GlCell::Program)};
        uint32_t index = u32At(a, 1);
        auto info = live(c)->getTransformFeedbackVarying(prog, index);
        if (info.type == 0) return ev::null();
        ObjectBuilder o;
        o.set("name", ev::fromUtf8(info.name));
        o.set("type", ev::fromDouble(info.type));
        o.set("size", ev::fromDouble(info.size));
        return o.get();
    });
    b.def("isTransformFeedback", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(live(c)->isTransformFeedback(transformFeedbackOf(argAt(a, 0))) != GL_FALSE);
    });
}

}  // namespace bro::bronze_host
