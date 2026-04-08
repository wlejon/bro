#pragma once

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
    /// Returns nullptr if val is not a Mesh instance.
    static bromesh::MeshData* getMeshData(JSContext* ctx, JSValueConst val);

    /// Get the class ID for Mesh (used by scene_bindings for instanceof checks).
    static JSClassID classId();
};

} // namespace bro::js
