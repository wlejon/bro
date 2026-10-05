#pragma once

// window.postMessage across the page's realms (host_window_message.cpp): the
// global itself, and the parse / clone / deliver steps a secondary window and
// an iframe share with it.

#include "embed/embed.h"

#include <cstdint>
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
void cloneForPostMessage(const ev::Persistent& message, const ev::Persistent& transfer,
                         ev::Persistent& dataOut, ev::Persistent& portsOut);
// Deliver a `message` MessageEvent at one window: the realm `scopeId` (0 =
// main; a window host id; an iframe document's scope) whose document is
// `doc` (ignored for 0). `onmessage` first, then that document's window
// listeners. A window that has gone away receives nothing. ALLOCATES.
void deliverWindowMessageEvent(uint64_t scopeId, dom::Document* doc, const ev::Persistent& data,
                               const ev::Persistent& ports, const ev::Persistent& source,
                               const std::string& origin);

}  // namespace bro::bronze_host
