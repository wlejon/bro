// Textures — the bronze twin of src/js/webgl2_bindings_textures.cpp for the
// 2D + cube-map surface three.js's texture system drives: bind/params/upload/
// mipmap/texStorage2D, plus the compressed 2D uploads a KTX/DDS loader feeds.
//
// The 6-arg DOM-source overloads (texImage2D(target, level, internalformat,
// format, type, image)) are NOT here: a bronze host has no Image or
// ImageBitmap values to source from yet, so only the 9-arg typed-array/null
// forms exist, and a 6-arg call is refused by name rather than silently
// misread as the 9-arg form.
//
// Upload pointers come from embed::typedArrayInfo and live only until the
// next bronze allocation — each is handed to its GL call (which copies into
// the driver) in the same statement chain with nothing allocating in between.

#include "bronze_host/gl_internal.h"

#include "util/log.h"

namespace bro::bronze_host {

void installGlTextures(ObjectBuilder& b, webgl::WebGL2RenderingContext* c) {
    b.def("createTexture", 0, [c](Value, std::span<const Value>) {
        return wrapGlObj(GlCell::Texture, live(c)->createTexture().id);
    });
    b.def("deleteTexture", 1, [c](Value, std::span<const Value> a) {
        live(c)->deleteTexture({idOf(argAt(a, 0), GlCell::Texture)});
        return ev::undefined();
    });
    b.def("bindTexture", 2, [c](Value, std::span<const Value> a) {
        live(c)->bindTexture(u32At(a, 0), {idOf(argAt(a, 1), GlCell::Texture)});
        return ev::undefined();
    });
    b.def("isTexture", 1, [c](Value, std::span<const Value> a) {
        return ev::fromBool(live(c)->isTexture({idOf(argAt(a, 0), GlCell::Texture)}) !=
                            GL_FALSE);
    });
    b.def("activeTexture", 1, [c](Value, std::span<const Value> a) {
        live(c)->activeTexture(u32At(a, 0));
        return ev::undefined();
    });
    b.def("texParameteri", 3, [c](Value, std::span<const Value> a) {
        live(c)->texParameteri(u32At(a, 0), u32At(a, 1), i32At(a, 2));
        return ev::undefined();
    });
    b.def("texParameterf", 3, [c](Value, std::span<const Value> a) {
        live(c)->texParameterf(u32At(a, 0), u32At(a, 1), static_cast<float>(numAt(a, 2)));
        return ev::undefined();
    });

    // texImage2D(target, level, internalformat, width, height, border,
    //            format, type, data|null). Data is a typed array or
    // null/undefined; a 6-arg DOM-source call is a named refusal (above).
    b.def("texImage2D", 9, [c](Value, std::span<const Value> a) {
        // Short calls are padded to arity with undefined, so a 6-arg DOM-source
        // call announces itself by an OBJECT where the 9-arg form has `border`
        // (a number). Refuse it by name instead of misreading it.
        if (ev::isObject(argAt(a, 5))) {
            LOG_ERROR("bronze_host: texImage2D 6-arg (DOM source) overload is not bound; "
                      "use the 9-arg typed-array form");
            return ev::undefined();
        }
        GLenum target = u32At(a, 0);
        GLint level = i32At(a, 1);
        GLint internalformat = i32At(a, 2);
        GLsizei width = i32At(a, 3);
        GLsizei height = i32At(a, 4);
        GLint border = i32At(a, 5);
        GLenum format = u32At(a, 6);
        GLenum type = u32At(a, 7);
        Value data = argAt(a, 8);
        if (ev::isUndefined(data) || ev::isNull(data)) {
            live(c)->texImage2D(target, level, internalformat, width, height, border,
                                format, type, nullptr);
        } else if (auto info = ev::typedArrayInfo(data)) {
            live(c)->texImage2D(target, level, internalformat, width, height, border,
                                format, type, info.data);
        }
        return ev::undefined();
    });

    // texSubImage2D(target, level, xoffset, yoffset, width, height, format,
    //               type, data). Same typed-array-only stance.
    b.def("texSubImage2D", 9, [c](Value, std::span<const Value> a) {
        // Same padding logic as texImage2D above: the 7-arg DOM-source call
        // puts an object where the 9-arg form has `format` — well, it puts its
        // source at index 6, where the 9-arg form has a numeric enum. An
        // object there is the 7-arg form; refuse it by name.
        if (ev::isObject(argAt(a, 6))) {
            LOG_ERROR("bronze_host: texSubImage2D 7-arg (DOM source) overload is not "
                      "bound; use the 9-arg typed-array form");
            return ev::undefined();
        }
        GLenum target = u32At(a, 0);
        GLint level = i32At(a, 1);
        GLint xoffset = i32At(a, 2);
        GLint yoffset = i32At(a, 3);
        GLsizei width = i32At(a, 4);
        GLsizei height = i32At(a, 5);
        GLenum format = u32At(a, 6);
        GLenum type = u32At(a, 7);
        if (auto info = ev::typedArrayInfo(argAt(a, 8))) {
            live(c)->texSubImage2D(target, level, xoffset, yoffset, width, height, format,
                                   type, info.data);
        }
        return ev::undefined();
    });

    b.def("texStorage2D", 5, [c](Value, std::span<const Value> a) {
        live(c)->texStorage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                              i32At(a, 4));
        return ev::undefined();
    });
    b.def("generateMipmap", 1, [c](Value, std::span<const Value> a) {
        live(c)->generateMipmap(u32At(a, 0));
        return ev::undefined();
    });

    // Compressed uploads, with the WebGL2 srcOffset/srcLengthOverride tail in
    // ELEMENT units of the source view — the same clamping the QuickJS
    // binding performs before the context's block-size validation runs.
    b.def("compressedTexImage2D", 7, [c](Value, std::span<const Value> a) {
        const uint8_t* data = nullptr;
        size_t len = 0, elemSize = 1;
        if (bufferBytes(argAt(a, 6), &data, &len, &elemSize)) {
            size_t elemCount = len / elemSize;
            size_t srcOffset = static_cast<size_t>(u32At(a, 7));
            if (srcOffset > elemCount) srcOffset = elemCount;
            size_t count = elemCount - srcOffset;
            if (a.size() >= 9 && !ev::isUndefined(a[8])) {
                size_t l = static_cast<size_t>(u32At(a, 8));
                if (l < count) count = l;
            }
            live(c)->compressedTexImage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2),
                                          i32At(a, 3), i32At(a, 4), i32At(a, 5),
                                          data + srcOffset * elemSize, count * elemSize);
        }
        return ev::undefined();
    });
    b.def("compressedTexSubImage2D", 8, [c](Value, std::span<const Value> a) {
        const uint8_t* data = nullptr;
        size_t len = 0, elemSize = 1;
        if (bufferBytes(argAt(a, 7), &data, &len, &elemSize)) {
            size_t elemCount = len / elemSize;
            size_t srcOffset = static_cast<size_t>(u32At(a, 8));
            if (srcOffset > elemCount) srcOffset = elemCount;
            size_t count = elemCount - srcOffset;
            if (a.size() >= 10 && !ev::isUndefined(a[9])) {
                size_t l = static_cast<size_t>(u32At(a, 9));
                if (l < count) count = l;
            }
            live(c)->compressedTexSubImage2D(u32At(a, 0), i32At(a, 1), i32At(a, 2),
                                             i32At(a, 3), i32At(a, 4), i32At(a, 5),
                                             u32At(a, 6), data + srcOffset * elemSize,
                                             count * elemSize);
        }
        return ev::undefined();
    });

    b.def("copyTexImage2D", 8, [c](Value, std::span<const Value> a) {
        live(c)->copyTexImage2D(u32At(a, 0), i32At(a, 1), u32At(a, 2), i32At(a, 3),
                                i32At(a, 4), i32At(a, 5), i32At(a, 6), i32At(a, 7));
        return ev::undefined();
    });
    b.def("copyTexSubImage2D", 8, [c](Value, std::span<const Value> a) {
        live(c)->copyTexSubImage2D(u32At(a, 0), i32At(a, 1), i32At(a, 2), i32At(a, 3),
                                   i32At(a, 4), i32At(a, 5), i32At(a, 6), i32At(a, 7));
        return ev::undefined();
    });
}

}  // namespace bro::bronze_host
