#pragma once

// brokit (host_brokit.cpp): the Node half — require, fs, path, os,
// child_process, process — and the web half — fetch, URL, Blob, encoding,
// base64, AbortController, WebSocket, streams, crypto, indexedDB, TreeWalker,
// EventTarget, MessageChannel. What it skips, and why, is at its top.

#include "embed/embed.h"

#include <string>

namespace bro::engine {
class Engine;
}  // namespace bro::engine

namespace bro::bronze_host {

namespace ev = bronze::embed;
using Value = bronze::Value;

void installBrokitGlobals(engine::Engine& engine);

// A brokit `File` read off disk (host_file_path.cpp): bytes, the MIME type
// its extension implies, `lastModified`, and the non-standard `.path` — the
// object a drop and an <input type=file> pick hand a page. `undefined` for
// a path that cannot be read. ALLOCATES.
Value makeFileFromPath(const std::string& path);

// The same, or for an unreadable path a plain `{name, path, size: 0, type}`
// so a file list never has a hole in it. ALLOCATES.
Value makeFileOrDescriptorFromPath(const std::string& path);

// One pass over brokit's polled completions (`__brokit_fetch_tick` and its
// siblings). hostFrame runs it as a host-task step; headless advanceTime and
// flush run it so a pending fetch resolves inside the call a test makes.
void pumpBrokitTicks();

// A microtask checkpoint that also runs, as the tasks that follow the current
// one, the local fetches (file:, data:, blob:) whose responses are already
// built: drain microtasks, settle those, drain again, until none are left or
// the turn's budget is spent. So a `fetch('templates/x.html')` a script makes
// resolves before the frame instead of at the next frame's brokit pump, while
// a long chain of them still yields to the frame (the pump settles the rest).
// `always` drains even with no microtask queued: bronze reports unhandled
// rejections at the end of a drain, so the frame's own checkpoints (hostFrame
// 6 and 6c) must run one every frame or a bare Promise.reject is never heard.
void drainMicrotasksAndLocalFetches(bool always = false);

// Whether brokit still has a fetch, socket or watcher in flight — the realm is
// not idle while it does (host_gc.cpp).
bool brokitHasPendingWork();

}  // namespace bro::bronze_host
