// State management + draw calls for WebGL2, each encoded by the context's
// client (nothing while the context is lost); isEnabled, getError and finish
// are round trips.
//
// Nothing in this file touches the bronze heap after decoding its arguments:
// the argument readers may allocate nothing, so no Value here ever goes
// stale mid-function.

#include "bronze_host/webgl_internal.h"

namespace bro::bronze_host {

void installWebGLState(ObjectBuilder& b) {
    defMethod(b, "viewport", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->viewport(i32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "scissor", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->scissor(i32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "clearColor", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->clearColor(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)),
                           static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    defMethod(b, "clearDepth", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->clearDepth(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });
    defMethod(b, "clearStencil", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->clearStencil(i32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "clear", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->clear(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "enable", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->enable(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "disable", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->disable(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "isEnabled", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        auto* cl = client(c);
        const GLenum cap = u32At(a, 0);
        return ev::fromBool(cl && cl->sync([cap](WebGLBackend& gl) { return gl.isEnabled(cap); }) != GL_FALSE);
    });
    defMethod(b, "depthFunc", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->depthFunc(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "depthMask", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->depthMask(boolAt(a, 0) ? GL_TRUE : GL_FALSE);
        return ev::undefined();
    });
    defMethod(b, "depthRange", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->depthRange(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    defMethod(b, "blendFunc", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->blendFunc(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    defMethod(b, "blendFuncSeparate", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->blendFuncSeparate(u32At(a, 0), u32At(a, 1), u32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "blendEquation", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->blendEquation(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "blendEquationSeparate", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->blendEquationSeparate(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    defMethod(b, "blendColor", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->blendColor(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)),
                           static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    defMethod(b, "colorMask", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->colorMask(boolAt(a, 0) ? GL_TRUE : GL_FALSE, boolAt(a, 1) ? GL_TRUE : GL_FALSE,
                          boolAt(a, 2) ? GL_TRUE : GL_FALSE, boolAt(a, 3) ? GL_TRUE : GL_FALSE);
        return ev::undefined();
    });
    defMethod(b, "stencilFunc", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->stencilFunc(u32At(a, 0), i32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });
    defMethod(b, "stencilFuncSeparate", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->stencilFuncSeparate(u32At(a, 0), u32At(a, 1), i32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "stencilOp", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->stencilOp(u32At(a, 0), u32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });
    defMethod(b, "stencilOpSeparate", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->stencilOpSeparate(u32At(a, 0), u32At(a, 1), u32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "stencilMask", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->stencilMask(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "stencilMaskSeparate", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->stencilMaskSeparate(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    defMethod(b, "cullFace", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->cullFace(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "frontFace", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->frontFace(u32At(a, 0));
        return ev::undefined();
    });
    defMethod(b, "polygonOffset", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->polygonOffset(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    defMethod(b, "sampleCoverage", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->sampleCoverage(static_cast<float>(numAt(a, 0)), boolAt(a, 1) ? GL_TRUE : GL_FALSE);
        return ev::undefined();
    });
    defMethod(b, "lineWidth", 1, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->lineWidth(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });
    defMethod(b, "pixelStorei", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->pixelStorei(u32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });
    defMethod(b, "getError", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        auto* cl = client(c);
        return ev::fromDouble(cl ? cl->sync([](WebGLBackend& gl) { return gl.getError(); }) : c->takeLostError());
    });
    defMethod(b, "flush", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        if (auto* cl = client(c)) cl->flush();
        return ev::undefined();
    });
    defMethod(b, "finish", 0, [](webgl::WebGL2RenderingContext* c, std::span<const Value>) {
        if (auto* cl = client(c)) cl->finish();
        return ev::undefined();
    });
    defMethod(b, "hint", 2, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->hint(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });

    // --- Draw calls ---
    defMethod(b, "drawArrays", 3, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->drawArrays(u32At(a, 0), i32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    defMethod(b, "drawElements", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->drawElements(u32At(a, 0), i32At(a, 1), u32At(a, 2), static_cast<uintptr_t>(i64At(a, 3)));
        return ev::undefined();
    });
    defMethod(b, "drawArraysInstanced", 4, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c)) cl->drawArraysInstanced(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    defMethod(b, "drawElementsInstanced", 5, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->drawElementsInstanced(u32At(a, 0), i32At(a, 1), u32At(a, 2), static_cast<uintptr_t>(i64At(a, 3)),
                                      i32At(a, 4));
        return ev::undefined();
    });
    defMethod(b, "drawRangeElements", 6, [](webgl::WebGL2RenderingContext* c, std::span<const Value> a) {
        if (auto* cl = client(c))
            cl->drawRangeElements(u32At(a, 0), u32At(a, 1), u32At(a, 2), i32At(a, 3), u32At(a, 4),
                                  static_cast<uintptr_t>(i64At(a, 5)));
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
