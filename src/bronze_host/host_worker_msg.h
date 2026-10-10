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

// An ImageBitmap in a message: its pixels by reference (they are immutable),
// so neither the post nor the receipt copies them.
struct SerializedImage {
    int width = 0;
    int height = 0;
    std::shared_ptr<const render::DecodedImage> pixels;
    // A big bitmap transferred out of a worker starts its texture upload as
    // it is posted (render/gpu_image_upload.h), so the copy overlaps the hop
    // to the page; the page's bitmap adopts it.
    std::shared_ptr<render::GpuImageUpload> upload;
};

// A malloc'd block of ArrayBuffer bytes the message owns until a reader
// adopts it: the receiving realm wraps it as an external ArrayBuffer
// (createExternalArrayBuffer) with no copy, and bronze frees it when that
// buffer is collected. Move-only.
struct ByteBlock {
    uint8_t* data = nullptr;
    uint32_t size = 0;

    ByteBlock() = default;
    ByteBlock(const uint8_t* src, uint32_t n);  // copies n bytes
    ByteBlock(ByteBlock&& o) noexcept : data(o.data), size(o.size) { o.data = nullptr; o.size = 0; }
    ByteBlock& operator=(ByteBlock&& o) noexcept;
    ByteBlock(const ByteBlock&) = delete;
    ByteBlock& operator=(const ByteBlock&) = delete;
    ~ByteBlock();
};

struct Message {
    std::vector<uint8_t> data;
    // Out-of-band ArrayBuffer bytes: every transferred buffer, and a copied
    // one past the inline threshold (CloneTarget::Local only). Consumed by
    // deserializeMessage, one-shot like a transfer — hence mutable.
    mutable std::vector<ByteBlock> buffers;
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
//
// `target` Local (a Worker, a window, anything read back in this process)
// may move ArrayBuffer bytes out of band; Wire (bro.net) keeps every byte in
// `data`, so the message is self-contained, and refuses transfers.
enum class CloneTarget { Local, Wire };
bool serializeMessage(Value val, std::span<const Value> transfers, Message& out,
                      CloneTarget target = CloneTarget::Local);
// The reader. Malformed data throws a TypeError into the program; a caller
// outside any call from JS (an event-loop drain) catches it with
// ev::catchThrow.
Value deserializeMessage(const Message& msg, size_t offset = 0);

// The transfer list of a postMessage call — `args[index]`, an array-like —
// read element by element into roots. Empty when the argument is absent or
// not an object.
std::vector<ev::Persistent> collectTransferList(std::span<const Value> args, size_t index = 1);

// The roots' current values. Allocates nothing on the JS heap, so the result
// is current until the next call that does.
std::vector<Value> currentValues(const std::vector<ev::Persistent>& roots);

} // namespace bro::bronze_host
