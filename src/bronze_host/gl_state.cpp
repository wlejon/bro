// State management + draw calls — the bronze twin of
// src/js/webgl2_bindings_state.cpp, function for function. Every entry routes
// through live(c) (makeCurrent) for the same reason every QuickJS entry
// routes through getCtx(): a call issued after another canvas or the engine's
// compositor touched GL must land in THIS context's framebuffer and state.
//
// Nothing in this file touches the bronze heap after decoding its arguments:
// the argument readers may allocate nothing, so no Value here ever goes
// stale mid-function.

#include "bronze_host/gl_internal.h"

namespace bro::bronze_host {

void installGlState(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    b.def("viewport", 4, [c](Value, std::span<const Value> a) {
        live(c)->viewport(i32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    b.def("scissor", 4, [c](Value, std::span<const Value> a) {
        live(c)->scissor(i32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    b.def("clearColor", 4, [c](Value, std::span<const Value> a) {
        live(c)->clearColor(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)),
                            static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    b.def("clearDepth", 1, [c](Value, std::span<const Value> a) {
        live(c)->clearDepth(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });
    b.def("clearStencil", 1, [c](Value, std::span<const Value> a) {
        live(c)->clearStencil(i32At(a, 0));
        return ev::undefined();
    });
    b.def("clear", 1, [c](Value, std::span<const Value> a) {
        live(c)->clear(u32At(a, 0));
        return ev::undefined();
    });
    b.def("enable", 1, [c](Value, std::span<const Value> a) {
        live(c)->enable(u32At(a, 0));
        return ev::undefined();
    });
    b.def("disable", 1, [c](Value, std::span<const Value> a) {
        live(c)->disable(u32At(a, 0));
        return ev::undefined();
    });
    b.def("isEnabled", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(live(c)->isEnabled(u32At(a, 0)) != GL_FALSE);
    });
    b.def("depthFunc", 1, [c](Value, std::span<const Value> a) {
        live(c)->depthFunc(u32At(a, 0));
        return ev::undefined();
    });
    b.def("depthMask", 1, [c](Value, std::span<const Value> a) {
        live(c)->depthMask(boolAt(a, 0) ? GL_TRUE : GL_FALSE);
        return ev::undefined();
    });
    b.def("depthRange", 2, [c](Value, std::span<const Value> a) {
        live(c)->depthRange(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    b.def("blendFunc", 2, [c](Value, std::span<const Value> a) {
        live(c)->blendFunc(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    b.def("blendFuncSeparate", 4, [c](Value, std::span<const Value> a) {
        live(c)->blendFuncSeparate(u32At(a, 0), u32At(a, 1), u32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    b.def("blendEquation", 1, [c](Value, std::span<const Value> a) {
        live(c)->blendEquation(u32At(a, 0));
        return ev::undefined();
    });
    b.def("blendEquationSeparate", 2, [c](Value, std::span<const Value> a) {
        live(c)->blendEquationSeparate(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    b.def("blendColor", 4, [c](Value, std::span<const Value> a) {
        live(c)->blendColor(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)),
                            static_cast<float>(numAt(a, 2)), static_cast<float>(numAt(a, 3)));
        return ev::undefined();
    });
    b.def("colorMask", 4, [c](Value, std::span<const Value> a) {
        live(c)->colorMask(boolAt(a, 0) ? GL_TRUE : GL_FALSE, boolAt(a, 1) ? GL_TRUE : GL_FALSE,
                           boolAt(a, 2) ? GL_TRUE : GL_FALSE, boolAt(a, 3) ? GL_TRUE : GL_FALSE);
        return ev::undefined();
    });
    b.def("stencilFunc", 3, [c](Value, std::span<const Value> a) {
        live(c)->stencilFunc(u32At(a, 0), i32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });
    b.def("stencilFuncSeparate", 4, [c](Value, std::span<const Value> a) {
        live(c)->stencilFuncSeparate(u32At(a, 0), u32At(a, 1), i32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    b.def("stencilOp", 3, [c](Value, std::span<const Value> a) {
        live(c)->stencilOp(u32At(a, 0), u32At(a, 1), u32At(a, 2));
        return ev::undefined();
    });
    b.def("stencilOpSeparate", 4, [c](Value, std::span<const Value> a) {
        live(c)->stencilOpSeparate(u32At(a, 0), u32At(a, 1), u32At(a, 2), u32At(a, 3));
        return ev::undefined();
    });
    b.def("stencilMask", 1, [c](Value, std::span<const Value> a) {
        live(c)->stencilMask(u32At(a, 0));
        return ev::undefined();
    });
    b.def("stencilMaskSeparate", 2, [c](Value, std::span<const Value> a) {
        live(c)->stencilMaskSeparate(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });
    b.def("cullFace", 1, [c](Value, std::span<const Value> a) {
        live(c)->cullFace(u32At(a, 0));
        return ev::undefined();
    });
    b.def("frontFace", 1, [c](Value, std::span<const Value> a) {
        live(c)->frontFace(u32At(a, 0));
        return ev::undefined();
    });
    b.def("polygonOffset", 2, [c](Value, std::span<const Value> a) {
        live(c)->polygonOffset(static_cast<float>(numAt(a, 0)), static_cast<float>(numAt(a, 1)));
        return ev::undefined();
    });
    b.def("lineWidth", 1, [c](Value, std::span<const Value> a) {
        live(c)->lineWidth(static_cast<float>(numAt(a, 0)));
        return ev::undefined();
    });
    b.def("pixelStorei", 2, [c](Value, std::span<const Value> a) {
        live(c)->pixelStorei(u32At(a, 0), i32At(a, 1));
        return ev::undefined();
    });
    b.def("getError", 0, [c](Value, std::span<const Value>) {
        return ev::fromDouble(live(c)->getError());
    });
    b.def("flush", 0, [c](Value, std::span<const Value>) {
        live(c)->flush();
        return ev::undefined();
    });
    b.def("finish", 0, [c](Value, std::span<const Value>) {
        live(c)->finish();
        return ev::undefined();
    });
    b.def("hint", 2, [c](Value, std::span<const Value> a) {
        live(c)->hint(u32At(a, 0), u32At(a, 1));
        return ev::undefined();
    });

    // --- Draw calls ---
    b.def("drawArrays", 3, [c](Value, std::span<const Value> a) {
        live(c)->drawArrays(u32At(a, 0), i32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    b.def("drawElements", 4, [c](Value, std::span<const Value> a) {
        live(c)->drawElements(u32At(a, 0), i32At(a, 1), u32At(a, 2),
                              static_cast<GLintptr>(i64At(a, 3)));
        return ev::undefined();
    });
    b.def("drawArraysInstanced", 4, [c](Value, std::span<const Value> a) {
        live(c)->drawArraysInstanced(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3));
        return ev::undefined();
    });
    b.def("drawElementsInstanced", 5, [c](Value, std::span<const Value> a) {
        live(c)->drawElementsInstanced(u32At(a, 0), i32At(a, 1), u32At(a, 2),
                                       static_cast<GLintptr>(i64At(a, 3)), i32At(a, 4));
        return ev::undefined();
    });
    b.def("drawRangeElements", 6, [c](Value, std::span<const Value> a) {
        live(c)->drawRangeElements(u32At(a, 0), u32At(a, 1), u32At(a, 2), i32At(a, 3),
                                   u32At(a, 4), static_cast<GLintptr>(i64At(a, 5)));
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
