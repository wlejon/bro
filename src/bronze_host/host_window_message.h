#pragma once

// window.postMessage across the page's realms (host_window_message.cpp): the
// global itself, and the parse / clone / deliver steps a secondary window and
// an iframe share with it.

#include "embed/embed.h"
#include "bronze_host/host_worker_msg.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace bro::dom {
class Document;
}  // namespace bro::dom

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

// `window.postMessage(message, targetOrigin, transfer)` for every realm
// (host_window_message.cpp): clones at the call, delivers a `message`
// MessageEvent at the CALLING realm's window as a later task. ALLOCATES.
Value makeWindowPostMessage();

// The origin every realm of the page has (`location.origin`).
inline constexpr const char* kHostPageOrigin = "bro://app";

// The parsed tail of a postMessage call: (message, targetOrigin, transfer),
// (message, {targetOrigin, transfer}) and — with `legacyTransferArray` —
// (message, transferArray). `deliver` is false when targetOrigin names
// another origin (the message is then dropped silently, per spec).
struct PostMessageTarget {
    std::string targetOrigin;
    ev::Persistent transfer;
    bool deliver = true;
};
// Throws a SyntaxError DOMException for a bad targetOrigin.
void parsePostMessageArgs(std::span<const Value> args, const char* what,
                          bool legacyTransferArray, PostMessageTarget& out);
// structuredClone `message` with `transfer`; the MessagePorts in the list come
// back as `portsOut` (an array). A clone error is thrown on to the caller.
// For a post to the caller's own window: the clone is of its realm.
void cloneForPostMessage(const ev::Persistent& message, const ev::Persistent& transfer,
                         ev::Persistent& dataOut, ev::Persistent& portsOut);
// The clone a post to ANOTHER compartment's window carries: bytes
// (host_worker_msg.h), read back in the receiver's realm, so every object the
// receiver gets is its own. A value that cannot be cloned is the caller's
// DataCloneError; MessagePorts do not cross compartments.
void serializeForPost(const ev::Persistent& message, const ev::Persistent& transfer, Message& out);
// Deliver a `message` MessageEvent at the window of `doc` (null: the app's),
// inside its compartment: `onmessage` first, then that document's window
// listeners. A same-window post hands its clone (`data`, `ports`, of that
// realm); a post from another compartment hands `msg`, read back there.
// `makeSource` runs inside the receiver's compartment and answers the
// event's `source`. A window that has gone away receives nothing. ALLOCATES.
void deliverWindowMessageEvent(dom::Document* doc, const ev::Persistent& data, const ev::Persistent& ports,
                               const std::function<Value()>& makeSource, const std::string& origin);
void deliverWindowMessage(dom::Document* doc, std::shared_ptr<const Message> msg,
                          const std::function<Value()>& makeSource, const std::string& origin);

}  // namespace bro::bronze_host
