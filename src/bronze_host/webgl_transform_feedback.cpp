// Transform feedback bindings for WebGL2.

#include "bronze_host/webgl_internal.h"

#include <string>
#include <vector>

namespace bro::bronze_host {

void installWebGLTransformFeedback(ObjectBuilder& b) {
    defMethod(b, "createTransformFeedback", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* gl = live(c);
        return gl ? webglObject(c, WebGLCell::TransformFeedback, gl->createTransformFeedback(c->newObjectId())) : ev::null();
    });
    defMethod(b, "deleteTransformFeedback", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint id = idOf(argAt(a, 0), WebGLCell::TransformFeedback);
        if (auto* gl = live(c)) gl->deleteTransformFeedback(id);
        forgetWebGLObject(c, WebGLCell::TransformFeedback, id);
        return ev::undefined();
    });
    defMethod(b, "bindTransformFeedback", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->bindTransformFeedback(u32At(a, 0), idOf(argAt(a, 1), WebGLCell::TransformFeedback));
        return ev::undefined();
    });
    defMethod(b, "beginTransformFeedback", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->beginTransformFeedback(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "endTransformFeedback", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->endTransformFeedback();
        return ev::undefined();
    });
    defMethod(b, "pauseTransformFeedback", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->pauseTransformFeedback();
        return ev::undefined();
    });
    defMethod(b, "resumeTransformFeedback", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->resumeTransformFeedback();
        return ev::undefined();
    });
    defMethod(b, "transformFeedbackVaryings", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        const GLuint prog = idOf(argAt(a, 0), WebGLCell::Program);
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
        if (auto* gl = live(c)) gl->transformFeedbackVaryings(prog, names, u32At(a, 2));
        return ev::undefined();
    });
    defMethod(b, "getTransformFeedbackVarying", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* gl = live(c);
        webgl::vk::VkFeedbackVarying info;
        if (!gl || !gl->getTransformFeedbackVarying(idOf(argAt(a, 0), WebGLCell::Program), u32At(a, 1), info))
            return ev::null();
        ObjectBuilder o;
        o.set("name", ev::fromUtf8(info.name));
        o.set("type", ev::fromDouble(info.type));
        o.set("size", ev::fromDouble(info.size));
        return o.get();
    });
    defMethod(b, "isTransformFeedback", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromBool(gl &&
                            gl->isTransformFeedback(idOf(argAt(a, 0), WebGLCell::TransformFeedback)) != GL_FALSE);
    });
}

}  // namespace bro::bronze_host
