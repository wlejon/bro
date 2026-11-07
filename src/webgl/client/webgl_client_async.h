#pragma once

// What the backend publishes for the client to read without asking it: the
// result of each link and compile, each query's status and result, each
// fence's queue ticket. One writer (the replay, wherever it runs) and one
// reader (the client, on the JS thread); no lock on either side.
//
// THE PUBLICATION RULE: a slot's data is written by the replay first, and a
// counter (a link's or compile's count, a query's serial, a fence's ticket)
// is stored after it with release. The client reads the counter with
// acquire and uses the data only when the counter says every call it
// encoded on that object has been replayed: from then until the client
// encodes another such call, nothing writes the data again, so the client
// reads it while the replay goes on with other commands.
//
// A record that needs its slot carries a reference to it (holdRef), which
// the replay takes back (takeRef), so a slot outlives a record the client
// forgot it for (an object deleted before its link replays).

#include "webgl/client/webgl_blob.h"
#include "webgl/webgl_objects.h"
#include "webgl/webgl_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace bro::webgl {

namespace vk {
class WebGLVkContext;
struct ProgramInterface;
}  // namespace vk

/// A shared_ptr carried in a record as a pointer.
template <class T>
void* holdRef(std::shared_ptr<T> p) {
    return p ? new std::shared_ptr<T>(std::move(p)) : nullptr;
}
template <class T>
std::shared_ptr<T> takeRef(void* held) {
    if (!held) return nullptr;
    std::unique_ptr<std::shared_ptr<T>> h(static_cast<std::shared_ptr<T>*>(held));
    return std::move(*h);
}

// --- Programs and shaders ---------------------------------------------------

/// What the last link of a program left: what getProgramParameter,
/// getProgramInfoLog and the interface queries answer until the next one.
/// A program never linked has the empty one.
struct ProgramLink {
    bool linked = false;
    std::string infoLog;
    std::shared_ptr<const vk::ProgramInterface> iface;  // never null
};

struct ProgramSlot {
    std::atomic<uint32_t> links{0};   // links replayed (and published)
    std::shared_ptr<const ProgramLink> link;  // the replay's to write; see the rule above
};

/// What the last compile of a shader left.
struct ShaderCompile {
    bool compiled = false;
    std::string infoLog;
};

struct ShaderSlot {
    std::atomic<uint32_t> compiles{0};
    std::shared_ptr<const ShaderCompile> compile;
};

/// The empty link, shared by every program before its first.
std::shared_ptr<const ProgramLink> emptyProgramLink();

/// The replay of linkProgram / compileShader: the call, then its result into
/// `slot` (null: nothing to publish).
void replayLinkProgram(vk::WebGLVkContext& gl, WebGLProgram p, ProgramSlot* slot);
void replayCompileShader(vk::WebGLVkContext& gl, WebGLShader s, ShaderSlot* slot);

// --- Queries ----------------------------------------------------------------

/// A query's status as getQueryParameter would find it now.
enum class QueryStatus : uint8_t {
    Refused = 0,    // not begun yet, or active: getQueryParameter raises (the backend says which)
    Pending = 1,    // ended, its result not on the host yet
    Available = 2,  // the result is `QuerySlot::result`
};

struct QuerySlot {
    /// serial << 8 | status: the status after the replay of the client's
    /// `serial`-th begin, end, or queryCounter on this query.
    std::atomic<uint64_t> word{0};
    std::atomic<uint64_t> result{0};  // stored before `word` names Available
};

// --- Fences -----------------------------------------------------------------

struct FenceSlot {
    /// The queue ticket the fence signals with, plus one; 0 until replayed.
    std::atomic<uint64_t> ticketPlus1{0};
};

// --- The replay's side ------------------------------------------------------

/// What the replay keeps besides the backend: blob accounting, and the
/// queries whose status it publishes after each batch of commands.
class WebGLReplayState {
public:
    /// Blob bytes outstanding (WebGLClient's cap), freed by the replay.
    std::atomic<size_t> blobBytes{0};
    /// Where payload blobs come from (the client) and go back to (the replay).
    WebGLBlobPool blobs;

    void createQuery(GLuint id, std::shared_ptr<QuerySlot> slot);
    void deleteQuery(GLuint id);
    /// A begin, end or queryCounter of the client's on `id` has replayed.
    void queryTouched(GLuint id);
    /// Publish what changed: every query not yet Available, or touched.
    /// After each batch of commands and each round trip.
    void publish(vk::WebGLVkContext& gl);
    /// The backend is gone (context loss, teardown): nothing to publish.
    void clear() { queries_.clear(); }

private:
    struct QueryEntry {
        std::shared_ptr<QuerySlot> slot;
        uint32_t serial = 0;
        uint32_t publishedSerial = 0;
        QueryStatus published = QueryStatus::Refused;
    };
    std::unordered_map<GLuint, QueryEntry> queries_;
};

}  // namespace bro::webgl
