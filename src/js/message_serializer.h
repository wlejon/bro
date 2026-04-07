#pragma once

#include "js/message_queue.h"

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

/// Serialize a JSValue into a Message using structured clone semantics.
/// transferList is a JS Array of ArrayBuffers to transfer (zero-copy),
/// or JS_UNDEFINED for no transfers.
/// Returns true on success. On failure, throws a JS exception and returns false.
bool serializeMessage(JSContext* ctx, JSValue value, JSValue transferList, Message& out);

/// Deserialize a Message back into a JSValue on the target context.
/// Returns the deserialized value, or JS_EXCEPTION on error.
JSValue deserializeMessage(JSContext* ctx, const Message& msg);

}  // namespace bro::js
