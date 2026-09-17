#pragma once

#include "bronze_host/host_globals_internal.h"
#include <memory>
#include <span>
#include <string>
#include <vector>

#if BRO_WITH_3D
#include <bromesh/mesh_data.h>
#endif

namespace bro::bronze_host {

struct SerializedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

struct Message {
    std::vector<uint8_t> data;
    std::vector<std::vector<uint8_t>> transferredBuffers;
    std::vector<SerializedImage> transferredImages;
#if BRO_WITH_3D
    // A Mesh listed in the transfer list crosses by pointer: the sender's
    // handle is left empty and the receiver's realm mints a Mesh of its own
    // class over the data (bromesh::api::makeMeshValue). Consumed by
    // deserializeMessage, which is why the slots are mutable behind a const
    // message: a transfer is one-shot, like a detached ArrayBuffer.
    mutable std::vector<std::unique_ptr<bromesh::MeshData>> transferredMeshes;
#endif
};

bool serializeMessage(Value val, std::span<const Value> transfers, Message& out);
Value deserializeMessage(const Message& msg, size_t offset = 0);

} // namespace bro::bronze_host
