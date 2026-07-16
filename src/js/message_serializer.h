#pragma once

#include "js/message_queue.h"

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

/// Serialize a JSValue into a Message using structured clone semantics.
/// transferList is a JS Array of ArrayBuffers to transfer (zero-copy),
/// or JS_UNDEFINED for no transfers.
/// forNetwork: the payload will leave the process (bro.net sendClone).
/// Pointer-transfer types (Mesh, ImageBitmap) cannot cross a network — they
/// throw a TypeError instead of being encoded as in-process pointer slots.
/// Returns true on success. On failure, throws a JS exception and returns false.
bool serializeMessage(JSContext* ctx, JSValue value, JSValue transferList, Message& out,
                      bool forNetwork = false);

/// Deserialize a Message back into a JSValue on the target context.
/// The message is passed by non-const reference because deserialization may
/// move ownership of transferred C++ objects out of it (e.g. Mesh transfers).
/// offset: byte offset into msg.data where the clone payload starts (bro.net
/// frames prepend a wire header). All reads are bounds-checked and recursion
/// is depth-limited, so untrusted network payloads produce a JS exception,
/// never a crash or out-of-bounds read.
/// Returns the deserialized value, or JS_EXCEPTION on error.
JSValue deserializeMessage(JSContext* ctx, Message& msg, size_t offset = 0);

}  // namespace bro::js
