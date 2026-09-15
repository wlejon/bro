// A `File` for a path on disk — what a drop and an <input type=file> pick
// hand a page. The class is brokit's (`File`, host_brokit.cpp installs it),
// so a dropped file and one built by `new File([...], name)` are the same
// kind of object down to `instanceof`; what this adds is the read off disk,
// the MIME type the extension implies, and the non-standard `.path`. Not a
// web property, and deliberately kept: a drop or a pick is the one moment a
// page is handed a real filesystem path (docs/paths-api.js, docs/file-api.js).

#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"

#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

std::string mimeForName(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return "";
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::pair<const char*, const char*> kTable[] = {
        {"png", "image/png"},    {"jpg", "image/jpeg"},  {"jpeg", "image/jpeg"},
        {"gif", "image/gif"},    {"webp", "image/webp"}, {"bmp", "image/bmp"},
        {"svg", "image/svg+xml"},
        {"json", "application/json"}, {"js", "text/javascript"},
        {"mjs", "text/javascript"},   {"css", "text/css"},
        {"html", "text/html"},   {"txt", "text/plain"},  {"md", "text/plain"},
        {"wav", "audio/wav"},    {"mp3", "audio/mpeg"},  {"ogg", "audio/ogg"},
        {"webm", "video/webm"},  {"mp4", "video/mp4"},
        {"glb", "model/gltf-binary"}, {"gltf", "model/gltf+json"},
        {"zip", "application/zip"},   {"wasm", "application/wasm"},
    };
    for (const auto& [k, v] : kTable)
        if (ext == k) return v;
    return "";
}

}  // namespace

Value makeFileFromPath(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path fsPath(path);

    std::ifstream in(fsPath, std::ios::binary);
    if (!in) return ev::undefined();
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (in.bad()) return ev::undefined();

    ev::GlobalValue fileCtor = ev::globalValue("File");
    if (!fileCtor.found || !ev::isFunction(fileCtor.value)) return ev::undefined();
    ev::Persistent ctor(fileCtor.value);

    const std::string name = fsPath.filename().string();
    // Best effort, and zero when the clock is unreadable: `lastModified` is
    // metadata a drop handler may print, never something it branches on.
    double lastModified = 0.0;
    const auto mtime = std::filesystem::last_write_time(fsPath, ec);
    if (!ec) {
        lastModified = static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                mtime.time_since_epoch()).count());
    }

    // `new File([bytes], name, {type, lastModified})`, every allocation
    // rooted before the next one.
    ev::Persistent view(ev::createTypedArray(ev::elements::Uint8,
                                             static_cast<uint32_t>(bytes.size())));
    ev::fillTypedArray(view.get(), bytes);
    ev::Persistent parts(hostArrayOf(1, [&view](size_t) { return view.get(); }));
    ev::Persistent opts(ev::createObject());
    opts.set(ev::setProperty(opts.get(), "type", ev::fromUtf8(mimeForName(name))));
    opts.set(ev::setProperty(opts.get(), "lastModified", ev::fromDouble(lastModified)));
    ev::Persistent nameV(ev::fromUtf8(name));
    Value args[3] = {parts.get(), nameV.get(), opts.get()};
    ev::CallResult r = ev::construct(ctor.get(), std::span<const Value>(args, 3));
    if (r.thrown) {
        reportBronzeError("File from path", r.value);
        return ev::undefined();
    }
    ev::Persistent file(r.value);
    file.set(ev::setProperty(file.get(), "path", ev::fromUtf8(path)));
    return file.get();
}

Value makeFileOrDescriptorFromPath(const std::string& path) {
    Value f = makeFileFromPath(path);
    if (!ev::isUndefined(f)) return f;
    // Unreadable (a directory, a permission error, a path that only names a
    // file): a plain {name, path, size: 0} stands in so the list never has a
    // hole in it — it passes the shape checks a page makes and fails every
    // read, which is the honest answer for bytes that are not there.
    ObjectBuilder d;
    d.set("name", ev::fromUtf8(std::filesystem::path(path).filename().string()));
    d.set("path", ev::fromUtf8(path));
    d.set("size", ev::fromDouble(0));
    d.set("type", ev::fromUtf8(mimeForName(path)));
    return d.get();
}

}  // namespace bro::bronze_host
