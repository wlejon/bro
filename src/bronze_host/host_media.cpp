#include "bronze_host/host_media.h"
#include "bronze_host/host_builder.h"
#include "bronze_host/host_values.h"
#include "bronze_host/host_bro_namespaces.h"
#include "util/asset_path.h"

#if BRO_WITH_VIDEO
#include "video/media_analysis.h"
#include <cctype>
#include <cmath>
#include <string>
#include <vector>
#if BRO_WITH_AUDIO
#include "broaudio/io/audio_tags.h"
#endif
#endif

namespace bro::bronze_host {

#if BRO_WITH_VIDEO

namespace {

static bool hasScheme(const std::string& src) {
    size_t i = 0;
    while (i < src.size() && (std::isalpha(static_cast<unsigned char>(src[i])) ||
                              (i > 0 && (std::isdigit(static_cast<unsigned char>(src[i])) ||
                                         src[i] == '+' || src[i] == '-' || src[i] == '.'))))
        ++i;
    return i > 1 && src.compare(i, 3, "://") == 0;
}

static std::string resolveMediaPath(const std::string& src) {
    if (src.size() >= 2 && src[1] == ':') return src;
    if (!src.empty() && (src[0] == '/' || src[0] == '\\')) return src;
    if (hasScheme(src)) return src;
    return util::resolveAssetPath(src);
}

static int intOption(Value opts, const char* name, int fallback) {
    if (!ev::isObject(opts)) return fallback;
    Value v = ev::getProperty(opts, name);
    if (!ev::isUndefined(v) && !ev::isNull(v)) {
        return satCast<int>(ev::toDouble(v));
    }
    return fallback;
}

static double numOption(Value opts, const char* name, double fallback) {
    if (!ev::isObject(opts)) return fallback;
    Value v = ev::getProperty(opts, name);
    if (!ev::isUndefined(v) && !ev::isNull(v)) {
        return ev::toDouble(v);
    }
    return fallback;
}

static video::Window windowOption(Value opts) {
    const double from = numOption(opts, "from", 0.0);
    const double to = numOption(opts, "to", 0.0);
    video::Window w;
    if (from > 0.0) w.fromNs = static_cast<video::TimeNs>(from * 1e9);
    if (to > 0.0) w.toNs = static_cast<video::TimeNs>(to * 1e9);
    return w;
}

static Value js_media_peaks(Value, std::span<const Value> a) {
    if (a.empty()) return ev::throwTypeError("peaks(path, options)");
    std::string rawPath = ev::toUtf8(a[0]);
    std::string path = resolveMediaPath(rawPath);

    int buckets = a.size() >= 2 ? intOption(a[1], "buckets", 2048) : 2048;
    if (buckets < 1) buckets = 1;
    if (buckets > (1 << 20)) buckets = 1 << 20;
    video::Window window = a.size() >= 2 ? windowOption(a[1]) : video::Window{};

    video::AudioPeaks peaks;
    if (!video::analyzeAudioPeaks(path, buckets, peaks, window)) return ev::null();

    ObjectBuilder out;
    out.set("sampleRate", ev::fromDouble(static_cast<double>(peaks.sampleRate)));
    out.set("channels", ev::fromDouble(static_cast<double>(peaks.channels)));
    out.set("duration", ev::fromDouble(peaks.durationNs / 1e9));
    out.set("from", ev::fromDouble(peaks.fromNs / 1e9));
    out.set("to", ev::fromDouble(peaks.toNs / 1e9));
    out.set("buckets", ev::fromDouble(static_cast<double>(peaks.maxv.size())));
    out.set("min", makeFloat32Array(peaks.minv));
    out.set("max", makeFloat32Array(peaks.maxv));
    out.set("rms", makeFloat32Array(peaks.rms));
    return out.get();
}

static Value js_media_thumbnails(Value, std::span<const Value> a) {
    if (a.empty()) return ev::throwTypeError("thumbnails(path, options)");
    std::string rawPath = ev::toUtf8(a[0]);
    std::string path = resolveMediaPath(rawPath);

    int count = a.size() >= 2 ? intOption(a[1], "count", 24) : 24;
    int height = a.size() >= 2 ? intOption(a[1], "height", 72) : 72;
    count = count < 1 ? 1 : (count > 4096 ? 4096 : count);
    height = height < 1 ? 1 : (height > 2048 ? 2048 : height);
    video::Window window = a.size() >= 2 ? windowOption(a[1]) : video::Window{};

    video::ThumbnailStrip strip;
    if (!video::grabThumbnails(path, count, height, strip, window)) return ev::null();

    // Each array is made as it is stored, so none is held across another's
    // allocation.
    ObjectBuilder out;
    out.set("width", ev::fromDouble(strip.width));
    out.set("height", ev::fromDouble(strip.height));
    out.set("count", ev::fromDouble(strip.count));
    out.set("rotation", ev::fromDouble(strip.rotationDegrees));
    out.set("times", hostArrayOf(strip.times.size(), [&](size_t i) -> Value {
        return ev::fromDouble(strip.times[i] / 1e9);
    }));
    // Clamped bytes, as ImageData wants them: `new ImageData(strip.data, ...)`.
    Value data = ev::createTypedArray(ev::elements::Uint8Clamped,
                                      static_cast<uint32_t>(strip.rgba.size()));
    if (!strip.rgba.empty())
        ev::fillTypedArray(data, std::span<const uint8_t>(strip.rgba.data(), strip.rgba.size()));
    out.set("data", data);
    return out.get();
}

#if BRO_WITH_AUDIO
// bro.media.tags(path): a music file's tags, length and format, read by
// broaudio's tag reader (headers and tag blocks only, never the audio).
static Value js_media_tags(Value, std::span<const Value> a) {
    if (a.empty()) return ev::throwTypeError("tags(path)");
    const std::string path = resolveMediaPath(ev::toUtf8(a[0]));
    broaudio::AudioTags t;
    if (!broaudio::readAudioTags(path.c_str(), t)) return ev::null();

    // Each value is made as it is stored, so none is held across another's allocation.
    ObjectBuilder out;
    auto str = [&](const char* name, const std::string& s) { out.set(name, ev::fromUtf8(s)); };
    auto num = [&](const char* name, double v) { out.set(name, ev::fromDouble(v)); };
    str("title", t.title);
    str("artist", t.artist);
    str("album", t.album);
    str("albumArtist", t.albumArtist);
    num("track", t.track);
    num("trackTotal", t.trackTotal);
    num("disc", t.disc);
    num("discTotal", t.discTotal);
    num("year", t.year);
    str("genre", t.genre);
    num("duration", t.duration);
    num("sampleRate", t.sampleRate);
    num("channels", t.channels);
    num("bitrate", t.bitrate);
    str("codec", t.codec);
    str("container", t.container);
    if (t.hasPicture) {
        ObjectBuilder pic;
        pic.set("mime", ev::fromUtf8(t.picture.mime));
        pic.set("bytes", makeUint8Array(t.picture.bytes));
        out.set("picture", pic.get());
    } else {
        out.set("picture", ev::null());
    }
    return out.get();
}
#endif

} // namespace

Value makeBroMediaValue() {
    ObjectBuilder media;
    media.set("available", ev::fromBool(true));
    media.def("peaks", 2, js_media_peaks);
    media.def("thumbnails", 2, js_media_thumbnails);
#if BRO_WITH_AUDIO
    media.def("tags", 1, js_media_tags);
#endif
    return media.get();
}

#else

Value makeBroMediaValue() {
    return makeUnavailableNamespace("media", "BRO_WITH_VIDEO");
}

#endif

} // namespace bro::bronze_host
