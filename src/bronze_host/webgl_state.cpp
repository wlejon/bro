// State management + draw calls for WebGL2, each straight onto the
// context's backend (nothing while the context is lost).
//
// Nothing in this file touches the bronze heap after decoding its arguments:
// the argument readers may allocate nothing, so no Value here ever goes
// stale mid-function.

#include "bronze_host/webgl_internal.h"

namespace bro::bronze_host {

void installWebGLState(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    b.def("viewport", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->viewport(i32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    b.def("scissor", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->scissor(i32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    b.def("clearColor", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->clearColor(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)),
                           static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    b.def("clearDepth", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->clearDepth(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });
    b.def("clearStencil", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->clearStencil(i32At(a, 0));
        return ev::undefined();
    });
    b.def("clear", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->clear(u32At(a, 0));
        return ev::undefined();
    });
    b.def("enable", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->enable(u32At(a, 0));
        return ev::undefined();
    });
    b.def("disable", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->disable(u32At(a, 0));
        return ev::undefined();
    });
    b.def("isEnabled", 1, [c](Value, std::span<const Value> a) {
        auto* gl = live(c);
        return ev::fromBool(gl && gl->isEnabled(u32At(a, 0)) != GL_FALSE);
    });
    b.def("depthFunc", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->depthFunc(u32At(a, 0));
        return ev::undefined();
    });
    b.def("depthMask", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->depthMask(boolAt(a, 0) ? GL_TRUE : GL_FALSE);
        return ev::undefined();
    });
    b.def("depthRange", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->depthRange(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    b.def("blendFunc", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->blendFunc(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    b.def("blendFuncSeparate", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->blendFuncSeparate(u32At(a, 0), u32At(a, 1), u32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    b.def("blendEquation", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->blendEquation(u32At(a, 0));
        return ev::undefined();
    });
    b.def("blendEquationSeparate", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->blendEquationSeparate(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    b.def("blendColor", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->blendColor(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)),
                           static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    b.def("colorMask", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->colorMask(boolAt(a, 0) ? GL_TRUE : GL_FALSE, boolAt(a, 1) ? GL_TRUE : GL_FALSE,
                          boolAt(a, 2) ? GL_TRUE : GL_FALSE, boolAt(a, 3) ? GL_TRUE : GL_FALSE);
        return ev::undefined();
    });
    b.def("stencilFunc", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->stencilFunc(u32At(a, 0), i32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });
    b.def("stencilFuncSeparate", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->stencilFuncSeparate(u32At(a, 0), u32At(a, 1), i32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    b.def("stencilOp", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->stencilOp(u32At(a, 0), u32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });
    b.def("stencilOpSeparate", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->stencilOpSeparate(u32At(a, 0), u32At(a, 1), u32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    b.def("stencilMask", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->stencilMask(u32At(a, 0));
        return ev::undefined();
    });
    b.def("stencilMaskSeparate", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->stencilMaskSeparate(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    b.def("cullFace", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->cullFace(u32At(a, 0));
        return ev::undefined();
    });
    b.def("frontFace", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->frontFace(u32At(a, 0));
        return ev::undefined();
    });
    b.def("polygonOffset", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->polygonOffset(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    b.def("sampleCoverage", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->sampleCoverage(static_cast<float>(numAt(a, 0)), boolAt(a, 1) ? GL_TRUE : GL_FALSE);
        return ev::undefined();
    });
    b.def("lineWidth", 1, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->lineWidth(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });
    b.def("pixelStorei", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->pixelStorei(u32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });
    b.def("getError", 0, [c](Value, std::span<const Value>) {
        auto* gl = live(c);
        return ev::fromDouble(gl ? gl->getError() : c->takeLostError());
    });
    b.def("flush", 0, [c](Value, std::span<const Value>) {
        if (auto* gl = live(c)) gl->flush();
        return ev::undefined();
    });
    b.def("finish", 0, [c](Value, std::span<const Value>) {
        if (auto* gl = live(c)) gl->finish();
        return ev::undefined();
    });
    b.def("hint", 2, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->hint(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });

    // --- Draw calls ---
    b.def("drawArrays", 3, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->drawArrays(u32At(a, 0), i32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    b.def("drawElements", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->drawElements(u32At(a, 0), i32At(a, 1), u32At(a, 2), static_cast<uintptr_t>(i64At(a, 3)));
        return ev::undefined();
    });
    b.def("drawArraysInstanced", 4, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c)) gl->drawArraysInstanced(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    b.def("drawElementsInstanced", 5, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->drawElementsInstanced(u32At(a, 0), i32At(a, 1), u32At(a, 2), static_cast<uintptr_t>(i64At(a, 3)),
                                      i32At(a, 4));
        return ev::undefined();
    });
    b.def("drawRangeElements", 6, [c](Value, std::span<const Value> a) {
        if (auto* gl = live(c))
            gl->drawRangeElements(u32At(a, 0), u32At(a, 1), u32At(a, 2), i32At(a, 3), u32At(a, 4),
                                  static_cast<uintptr_t>(i64At(a, 5)));
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
