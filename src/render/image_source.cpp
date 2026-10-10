#include "render/image_source.h"

#include "util/asset_path.h"
#include "util/object_url.h"
#include "util/remote_asset.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace bro::render {

namespace {

// FNV-1a over a data: URL, so the key of a multi-megabyte inline image is not
// the image.
uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string orientSuffix(bool oriented) { return oriented ? "|o" : "|n"; }

bool readWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0);
    out.resize(static_cast<size_t>(size));
    if (size > 0) f.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(f) || f.eof();
}

}  // namespace

ImageSource resolveImageSource(const std::string& url, const std::string& basePath, bool oriented) {
    ImageSource src;
    src.oriented = oriented;
    if (url.empty()) {
        src.error = "empty URL";
        return src;
    }

    if (url.compare(0, 5, "data:") == 0) {
        if (url.find(',') == std::string::npos) {
            src.error = "malformed data: URL";
            return src;
        }
        src.kind = ImageSource::Kind::Data;
        src.url = url;
        char buf[64];
        std::snprintf(buf, sizeof buf, "data:%016llx:%zu", static_cast<unsigned long long>(fnv1a(url)),
                      url.size());
        src.key = std::string(buf) + orientSuffix(oriented);
        return src;
    }

    if (util::isObjectURL(url)) {
        auto data = util::lookupObjectURL(url);
        if (!data || data->bytes.empty()) {
            src.error = "the blob: URL is revoked or was never registered";
            return src;
        }
        src.kind = ImageSource::Kind::Bytes;
        src.bytes = std::shared_ptr<const std::vector<uint8_t>>(data, &data->bytes);
        src.key = url + orientSuffix(oriented);
        return src;
    }

    if (util::isHttpUrl(url)) {
        src.kind = ImageSource::Kind::Remote;
        src.url = url;
        src.key = url + orientSuffix(oriented);
        return src;
    }

    // A file. `thumbnails/a.png?v=12` is the standard cache-bust: the query
    // and fragment name no part of the file.
    std::string clean = url;
    if (const auto q = clean.find_first_of("?#"); q != std::string::npos) clean.resize(q);
    if (clean.compare(0, 7, "file://") == 0) {
        clean.erase(0, 7);
        // file:///C:/x -> C:/x
        if (clean.size() >= 3 && clean[0] == '/' && clean[2] == ':') clean.erase(0, 1);
    }
    std::string path;
    if (clean.size() >= 2 && clean[1] == ':') {
        path = clean;
    } else if (!clean.empty() && (clean[0] == '/' || clean[0] == '\\')) {
        path = util::resolveAssetPath(clean);
    } else if (!basePath.empty()) {
        path = basePath;
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += clean;
    } else {
        path = util::resolveAssetPath(clean);
    }

    std::error_code ec;
    const std::filesystem::path fp = std::filesystem::u8path(path);
    const auto size = std::filesystem::file_size(fp, ec);
    if (ec) {
        src.error = "cannot open " + path;
        return src;
    }
    const auto mtime = std::filesystem::last_write_time(fp, ec);
    const long long stamp = ec ? 0 : static_cast<long long>(mtime.time_since_epoch().count());
    src.kind = ImageSource::Kind::File;
    src.path = path;
    src.key = "file:" + path + "|" + std::to_string(size) + "|" + std::to_string(stamp) +
              orientSuffix(oriented);
    return src;
}

std::shared_ptr<ImageRequest> requestImage(const ImageSource& src) {
    if (src.kind == ImageSource::Kind::None || src.key.empty()) return nullptr;
    ImageStore& store = ImageStore::instance();
    if (auto cached = store.find(src.key)) return cached;

    ImageWork work;
    const bool orient = src.oriented;
    switch (src.kind) {
    case ImageSource::Kind::File:
        work = [path = src.path, orient](DecodedImage& out, std::string& err) {
            std::vector<uint8_t> bytes;
            if (!readWholeFile(path, bytes)) {
                err = "cannot read " + path;
                return false;
            }
            return decodeImageData(bytes.data(), bytes.size(), orient, out, err, /*animate=*/true);
        };
        break;
    case ImageSource::Kind::Bytes:
        work = [bytes = src.bytes, orient](DecodedImage& out, std::string& err) {
            return decodeImageData(bytes->data(), bytes->size(), orient, out, err, /*animate=*/true);
        };
        break;
    case ImageSource::Kind::Data:
        work = [url = src.url, orient](DecodedImage& out, std::string& err) {
            std::vector<uint8_t> bytes;
            if (!util::inlineURLBytes(url, bytes) || bytes.empty()) {
                err = "the data: URL carries no bytes";
                return false;
            }
            return decodeImageData(bytes.data(), bytes.size(), orient, out, err, /*animate=*/true);
        };
        break;
    case ImageSource::Kind::Remote:
        work = [url = src.url, orient](DecodedImage& out, std::string& err) {
            const std::string body = util::fetchRemoteCached(url);
            if (body.empty()) {
                err = "could not fetch " + url;
                return false;
            }
            return decodeImageData(reinterpret_cast<const uint8_t*>(body.data()), body.size(), orient,
                                   out, err, /*animate=*/true);
        };
        break;
    case ImageSource::Kind::None:
        return nullptr;
    }
    return store.request(src.key, std::move(work));
}

bool probeImageSource(const ImageSource& src, int& width, int& height, int& orientation,
                      bool& isSvg) {
    width = height = 0;
    orientation = 1;
    isSvg = false;
    switch (src.kind) {
    case ImageSource::Kind::File: {
        std::ifstream f(std::filesystem::u8path(src.path), std::ios::binary);
        if (!f) return false;
        // Header only: every format puts its size (and a JPEG its EXIF) in
        // the first few KB, so 64 KB covers them without reading the photo.
        std::vector<uint8_t> head(64 * 1024);
        f.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head.resize(static_cast<size_t>(f.gcount()));
        return probeImageData(head.data(), head.size(), src.oriented, width, height, orientation,
                              isSvg);
    }
    case ImageSource::Kind::Bytes:
        return probeImageData(src.bytes->data(), src.bytes->size(), src.oriented, width, height,
                              orientation, isSvg);
    case ImageSource::Kind::Data: {
        std::vector<uint8_t> bytes;
        if (!util::inlineURLBytes(src.url, bytes)) return false;
        return probeImageData(bytes.data(), bytes.size(), src.oriented, width, height, orientation,
                              isSvg);
    }
    case ImageSource::Kind::Remote:
    case ImageSource::Kind::None:
        return false;
    }
    return false;
}

}  // namespace bro::render
