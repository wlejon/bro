#pragma once

// The files a request or a reply of the agent inbox carries
// (docs/agent-inbox.md), as one byte string: shared by the engine
// (engine/agent_inbox.cpp) and bro-ctl, header-only over brolink's wire
// primitives like control_protocol.h.
//
//   varint count, then count × (str name, str bytes)
//
// It crosses the control protocol the way any file does there: going to the
// engine as the value of a `--files=` argument (an argv string is
// length-prefixed bytes, on the local socket and on broremote's text lane
// alike), and coming back as a reply's `file` through an output-path argument
// (the request's outArg), so a remote client gets the bytes, not a host path.
//
// A name is a file name only: no directory part, not "." or "..", no NUL. A
// receiver writes each file under a directory of its own choosing and never
// trusts the name further than that.

#include <brolink/wire.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bro::platform::inbox {

struct File {
    std::string name;
    std::string bytes;
};

/// The largest bundle either side builds or accepts: under the control
/// protocol's message limit, with room for the rest of the request.
inline constexpr size_t kMaxBundle = 48u << 20;

/// Whether `name` is a plain file name a receiver may create.
inline bool validName(std::string_view name) {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") return false;
    for (char c : name)
        if (c == '/' || c == '\\' || c == '\0' || c == ':') return false;
    return true;
}

/// The last path component of `path`, for naming a file by where it was read.
inline std::string baseName(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

inline std::string encode(const std::vector<File>& files) {
    brolink::wire::Writer w;
    w.varint(files.size());
    for (const File& f : files) {
        w.str(f.name);
        w.str(f.bytes);
    }
    return w.take();
}

/// False (with `why`) for a malformed bundle or a bad name.
inline bool decode(std::string_view body, std::vector<File>& out, std::string* why = nullptr) {
    out.clear();
    if (body.empty()) return true;  // no files
    if (body.size() > kMaxBundle) {
        if (why) *why = "the files are larger than " + std::to_string(kMaxBundle >> 20) + " MB";
        return false;
    }
    brolink::wire::Reader r(body);
    const uint64_t n = r.varint();
    if (!r.ok() || n > 4096) {
        if (why) *why = "malformed files";
        return false;
    }
    for (uint64_t i = 0; i < n; ++i) {
        File f;
        f.name = r.str();
        f.bytes = r.str();
        if (!r.ok()) break;
        if (!validName(f.name)) {
            if (why) *why = "bad file name '" + f.name + "' (a plain file name, no directory)";
            return false;
        }
        out.push_back(std::move(f));
    }
    if (!r.done()) {
        if (why) *why = "malformed files";
        out.clear();
        return false;
    }
    return true;
}

}  // namespace bro::platform::inbox
