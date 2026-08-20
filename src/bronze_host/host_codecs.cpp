// bro.mesh.decodeDraco / bro.mesh.encodeDraco / bro.image.transcodeKTX2 for
// the COMPILED realm — the same native codecs src/js/mesh_bindings.cpp and
// src/js/image_bindings.cpp expose to QuickJS. These exist because the web's
// answer to Draco and KTX2 is a Worker spinning a WASM decoder, and this
// stack has no WASM engine anywhere by design: bromesh carries google/draco
// and broimage carries the basis_universal transcoder as ordinary C++, so a
// loader is one synchronous call. The DRACOLoader/KTX2Loader shims an app's
// import map installs (three.js editor) are thin wrappers over these.

#include "bronze_host/gl_internal.h"  // ObjectBuilder
#include "bronze_host/host_internal.h"

#if defined(BROMESH_HAS_DRACO)
#include <bromesh/io/draco.h>
#endif
#if defined(BROIMAGE_HAS_KTX2)
#include <broimage/ktx2.h>
#endif

#include <cstring>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

// A compiled Uint8Array's (or any view's) window, copied out. Copy rather
// than alias: the decode allocates on the bronze heap, and embed's pointer
// contract voids the source pointer at the first allocation.
bool bytesOf(Value v, std::vector<uint8_t>& out) {
    ev::TypedArrayInfo info = ev::typedArrayInfo(v);
    if (!info) return false;
    out.assign(info.data, info.data + info.byteLength);
    return true;
}

Value typedArrayFrom(bronze::ElementKind kind, const void* data, size_t byteLength,
                     uint32_t elementCount) {
    Value arr = ev::createTypedArray(kind, elementCount);
    ev::fillTypedArray(arr, std::span<const uint8_t>(static_cast<const uint8_t*>(data),
                                                     byteLength));
    return arr;
}

#if defined(BROMESH_HAS_DRACO)

Value decodeDracoValue(Value, std::span<const Value> args) {
    std::vector<uint8_t> bytes;
    if (args.empty() || !bytesOf(args[0], bytes)) {
        return ev::throwTypeError("decodeDraco requires (bytes: a typed array)");
    }
    bromesh::DracoDecoded decoded = bromesh::decodeDraco(bytes.data(), bytes.size());
    if (!decoded.ok()) return ev::throwTypeError(decoded.error.c_str());

    ObjectBuilder out;
    const bromesh::MeshData& m = decoded.mesh;
    out.set("positions", typedArrayFrom(ev::elements::Float32, m.positions.data(),
                                        m.positions.size() * sizeof(float),
                                        static_cast<uint32_t>(m.positions.size())));
    if (!m.normals.empty()) {
        out.set("normals", typedArrayFrom(ev::elements::Float32, m.normals.data(),
                                          m.normals.size() * sizeof(float),
                                          static_cast<uint32_t>(m.normals.size())));
    }
    if (!m.uvs.empty()) {
        out.set("uvs", typedArrayFrom(ev::elements::Float32, m.uvs.data(),
                                      m.uvs.size() * sizeof(float),
                                      static_cast<uint32_t>(m.uvs.size())));
    }
    if (!m.colors.empty()) {
        out.set("colors", typedArrayFrom(ev::elements::Float32, m.colors.data(),
                                         m.colors.size() * sizeof(float),
                                         static_cast<uint32_t>(m.colors.size())));
    }
    if (!m.indices.empty()) {
        out.set("indices", typedArrayFrom(ev::elements::Uint32, m.indices.data(),
                                          m.indices.size() * sizeof(uint32_t),
                                          static_cast<uint32_t>(m.indices.size())));
    }

    // Every attribute raw — integer skinning data stays integer, and glTF
    // consumers key on uniqueId.
    out.set("attributes", hostArrayOf(decoded.attributes.size(), [&](size_t i) -> Value {
        const bromesh::DracoAttribute& a = decoded.attributes[i];
        bronze::ElementKind kind = ev::elements::Float32;
        const char* kindName = "float32";
        switch (a.kind) {
            case bromesh::DracoAttribute::Kind::Int8: kind = ev::elements::Int8; kindName = "int8"; break;
            case bromesh::DracoAttribute::Kind::Uint8: kind = ev::elements::Uint8; kindName = "uint8"; break;
            case bromesh::DracoAttribute::Kind::Int16: kind = ev::elements::Int16; kindName = "int16"; break;
            case bromesh::DracoAttribute::Kind::Uint16: kind = ev::elements::Uint16; kindName = "uint16"; break;
            case bromesh::DracoAttribute::Kind::Int32: kind = ev::elements::Int32; kindName = "int32"; break;
            case bromesh::DracoAttribute::Kind::Uint32: kind = ev::elements::Uint32; kindName = "uint32"; break;
            default: break;
        }
        ObjectBuilder attr;
        attr.set("type", ev::fromUtf8(a.type));
        attr.set("uniqueId", ev::fromDouble(a.uniqueId));
        attr.set("components", ev::fromDouble(a.components));
        attr.set("count", ev::fromDouble(a.count));
        attr.set("kind", ev::fromUtf8(kindName));
        attr.set("data", typedArrayFrom(kind, a.bytes.data(), a.bytes.size(),
                                        a.count * static_cast<uint32_t>(a.components)));
        return attr.get();
    }));
    return out.get();
}

Value encodeDracoValue(Value, std::span<const Value> args) {
    if (args.empty() || !ev::isObject(args[0])) {
        return ev::throwTypeError("encodeDraco requires ({positions, indices, ...})");
    }
    bromesh::MeshData mesh;
    auto readStream = [&](const char* name, std::vector<float>& into) {
        ev::TypedArrayInfo info = ev::typedArrayInfo(ev::getProperty(args[0], name));
        if (!info) return;
        into.resize(info.byteLength / sizeof(float));
        std::memcpy(into.data(), info.data, into.size() * sizeof(float));
    };
    readStream("positions", mesh.positions);
    readStream("normals", mesh.normals);
    readStream("uvs", mesh.uvs);
    readStream("colors", mesh.colors);
    if (ev::TypedArrayInfo info = ev::typedArrayInfo(ev::getProperty(args[0], "indices"))) {
        mesh.indices.resize(info.byteLength / sizeof(uint32_t));
        std::memcpy(mesh.indices.data(), info.data, mesh.indices.size() * sizeof(uint32_t));
    }

    bromesh::DracoEncodeOptions opt;
    if (args.size() > 1 && ev::isObject(args[1])) {
        auto readInt = [&](const char* name, int& into) {
            Value v = ev::getProperty(args[1], name);
            if (ev::isNumber(v)) into = static_cast<int>(ev::toDouble(v));
        };
        readInt("positionBits", opt.positionBits);
        readInt("normalBits", opt.normalBits);
        readInt("uvBits", opt.uvBits);
        readInt("colorBits", opt.colorBits);
        readInt("speed", opt.speed);
    }

    std::string error;
    std::vector<uint8_t> bytes = bromesh::encodeDraco(mesh, opt, &error);
    if (bytes.empty()) return ev::throwTypeError(error.c_str());
    return typedArrayFrom(ev::elements::Uint8, bytes.data(), bytes.size(),
                          static_cast<uint32_t>(bytes.size()));
}

#endif  // BROMESH_HAS_DRACO

#if defined(BROIMAGE_HAS_KTX2)

Value transcodeKtx2Value(Value, std::span<const Value> args) {
    std::vector<uint8_t> bytes;
    if (args.empty() || !bytesOf(args[0], bytes)) {
        return ev::throwTypeError("transcodeKTX2 requires (bytes: a typed array)");
    }
    broimage::Ktx2Format target = broimage::Ktx2Format::RGBA8;
    if (args.size() > 1 && ev::isString(args[1])) {
        const std::string f = ev::toUtf8(args[1]);
        if (f == "bc1") target = broimage::Ktx2Format::BC1;
        else if (f == "bc3") target = broimage::Ktx2Format::BC3;
        else if (f == "bc4") target = broimage::Ktx2Format::BC4;
        else if (f == "bc5") target = broimage::Ktx2Format::BC5;
        else if (f == "bc7") target = broimage::Ktx2Format::BC7;
        else if (f != "rgba8")
            return ev::throwTypeError(("transcodeKTX2: unknown format '" + f + "'").c_str());
    }

    broimage::Ktx2Image img = broimage::transcode_ktx2(bytes.data(), bytes.size(), target);
    if (!img.ok()) return ev::throwTypeError(img.error.c_str());

    const char* formatName = "rgba8";
    switch (img.format) {
        case broimage::Ktx2Format::BC1: formatName = "bc1"; break;
        case broimage::Ktx2Format::BC3: formatName = "bc3"; break;
        case broimage::Ktx2Format::BC4: formatName = "bc4"; break;
        case broimage::Ktx2Format::BC5: formatName = "bc5"; break;
        case broimage::Ktx2Format::BC7: formatName = "bc7"; break;
        default: break;
    }

    ObjectBuilder out;
    out.set("width", ev::fromDouble(img.width));
    out.set("height", ev::fromDouble(img.height));
    out.set("hasAlpha", ev::fromBool(img.hasAlpha));
    out.set("srgb", ev::fromBool(img.srgb));
    out.set("format", ev::fromUtf8(formatName));
    out.set("mips", hostArrayOf(img.mips.size(), [&](size_t i) -> Value {
        const broimage::Ktx2Level& level = img.mips[i];
        ObjectBuilder mip;
        mip.set("width", ev::fromDouble(level.width));
        mip.set("height", ev::fromDouble(level.height));
        mip.set("data", typedArrayFrom(ev::elements::Uint8, level.data.data(),
                                       level.data.size(),
                                       static_cast<uint32_t>(level.data.size())));
        return mip.get();
    }));
    return out.get();
}

#endif  // BROIMAGE_HAS_KTX2

}  // namespace

Value makeBroMeshValue() {
    ObjectBuilder b;
#if defined(BROMESH_HAS_DRACO)
    b.def("decodeDraco", 1, decodeDracoValue);
    b.def("encodeDraco", 2, encodeDracoValue);
#endif
    return b.get();
}

Value makeBroImageValue() {
    ObjectBuilder b;
#if defined(BROIMAGE_HAS_KTX2)
    b.def("transcodeKTX2", 2, transcodeKtx2Value);
#endif
    return b.get();
}

}  // namespace bro::bronze_host
