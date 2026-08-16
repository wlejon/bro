#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::util {

// Object URLs — the `blob:` URLs `URL.createObjectURL(blob)` hands out.
//
// On the web an object URL is a name for some bytes the page already holds, and
// every URL consumer resolves it: <img src>, fetch, <video>, a worker. In bro
// most of those consumers are C++ and do not have (or want) a JSContext, so the
// bytes are copied out of the Blob when the URL is minted and kept here, in one
// process-global table, until the page revokes it.
//
// Copying is deliberate. The alternative is holding the Blob alive from C++ and
// reaching into JS to read it, which the draw path — running on the layout
// thread, mid-frame — cannot do. Object URLs are how a page hands the runtime a
// generated texture or an imported model's embedded assets, so the copy buys a
// resource every consumer can read from any thread.
struct ObjectURLData {
    std::vector<uint8_t> bytes;
    std::string type;      // the Blob's MIME type; may be empty
};

// True for a string that looks like an object URL at all — cheap prefix test
// for consumers deciding whether a lookup is worth doing.
bool isObjectURL(const std::string& url);

// Take ownership of `bytes` under `url`, replacing any previous entry.
void registerObjectURL(const std::string& url, std::vector<uint8_t> bytes,
                       std::string type);

// Drop `url`. Consumers holding a shared_ptr from lookupObjectURL keep reading
// their copy until they let go, so a revoke mid-decode is safe.
void revokeObjectURL(const std::string& url);

// The bytes behind `url`, or nullptr if it was never registered or has been
// revoked. Safe from any thread.
std::shared_ptr<const ObjectURLData> lookupObjectURL(const std::string& url);

// Bytes for a URL that carries its own payload rather than naming a file: a
// data: URL's inline body, or a blob: URL's registered bytes. Returns false —
// leaving `out` alone — for anything else, which the caller should go on to
// resolve as a path.
//
// Both schemes together, because every consumer of an image or asset URL has
// the same question at the same moment ("can I read this without touching the
// disk?"), and answering it in each of them separately is how <img src="data:">
// came to work in the layout walk but not in `new Image()`.
bool inlineURLBytes(const std::string& url, std::vector<uint8_t>& out,
                    std::string* mime = nullptr);

} // namespace bro::util
