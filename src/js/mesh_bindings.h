#pragma once

#include <memory>

extern "C" {
#include "quickjs.h"
}

namespace bromesh { struct MeshData; }

namespace bro::js {

class MeshBindings {
public:
    /// Install the Mesh class constructor/prototype into the JS context.
    static void install(JSContext* ctx);

    /// Clean up class IDs and prototypes.
    static void cleanup(JSContext* ctx);

    /// Extract the internal MeshData from a JS Mesh object.
    /// Returns nullptr if val is not a Mesh instance or it has been neutered.
    static bromesh::MeshData* getMeshData(JSContext* ctx, JSValueConst val);

    /// Transfer ownership of the internal MeshData out of a JS Mesh, leaving
    /// the source Mesh neutered (getMeshData will return nullptr afterward).
    /// Returns an empty unique_ptr if val is not a Mesh or is already neutered.
    /// Used by the postMessage serializer to move meshes across threads with
    /// no copy through the JS heap.
    static std::unique_ptr<bromesh::MeshData> takeMeshData(JSContext* ctx, JSValueConst val);

    /// Wrap an existing MeshData into a fresh JS Mesh on the given context.
    /// Used by the postMessage deserializer to rehydrate a transferred mesh
    /// on the destination thread.
    static JSValue wrapMeshData(JSContext* ctx, std::unique_ptr<bromesh::MeshData> data);

    /// Get the class ID for Mesh (used by scene_bindings for instanceof checks).
    static JSClassID classId();
};

} // namespace bro::js
