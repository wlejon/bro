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
    bool isError = false;
    std::string errorMessage;
    std::string errorFilename;
    int errorLineno = 0;
#if BRO_WITH_3D
    // A Mesh listed in the transfer list crosses by pointer: the sender's
    // handle is left empty and the receiver's realm mints a Mesh of its own
    // class over the data (bromesh::api::makeMeshValue). Consumed by
    // deserializeMessage, which is why the slots are mutable behind a const
    // message: a transfer is one-shot, like a detached ArrayBuffer.
    mutable std::vector<std::unique_ptr<bromesh::MeshData>> transferredMeshes;
#endif
};

// The structured-clone writer. `val` and every entry of `transfers` need only
// be current at entry — both are rooted before the first allocation, since
// cloning runs getters and builtins that allocate. A caller that collects the
// transfer list itself must keep the entries in Persistents while it does
// (collectTransferList), because each element read can move the earlier ones,
// and read them out (currentValues) only in the statement before the call.
bool serializeMessage(Value val, std::span<const Value> transfers, Message& out);
Value deserializeMessage(const Message& msg, size_t offset = 0);

// The transfer list of a postMessage call — `args[index]`, an array-like —
// read element by element into roots. Empty when the argument is absent or
// not an object.
std::vector<ev::Persistent> collectTransferList(std::span<const Value> args, size_t index = 1);

// The roots' current values. Allocates nothing on the JS heap, so the result
// is current until the next call that does.
std::vector<Value> currentValues(const std::vector<ev::Persistent>& roots);

} // namespace bro::bronze_host
