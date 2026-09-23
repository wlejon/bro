#include "bronze_host/bronze_host.h"
#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "engine/engine.h"
#include "render/bidi.h"
#include "render/renderer.h"
#include "render/shaped_run.h"

#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

struct Opts {
    std::string family = "Arial";
    render::FontRef ref{};
    render::Spacing spacing{};
};

static Opts readOpts(Value vIn) {
    Opts o;
    if (ev::isObject(vIn)) {
        const Rooted v(vIn);  // each property read allocates
        Value famVal = ev::getProperty(v, "family");
        if (ev::isString(famVal)) {
            std::string fam = ev::toUtf8(famVal);
            if (!fam.empty()) o.family = std::move(fam);
        }
        Value szVal = ev::getProperty(v, "size");
        o.ref.size = ev::isNumber(szVal) ? static_cast<float>(ev::toDouble(szVal)) : 16.0f;
        Value wVal = ev::getProperty(v, "weight");
        o.ref.weight = ev::isNumber(wVal) ? static_cast<int>(ev::toDouble(wVal)) : 400;
        Value itVal = ev::getProperty(v, "italic");
        o.ref.italic = ev::isBool(itVal) ? ev::toBool(itVal) : false;
        Value lsVal = ev::getProperty(v, "letterSpacing");
        o.spacing.letter = ev::isNumber(lsVal) ? static_cast<float>(ev::toDouble(lsVal)) : 0.0f;
        Value wsVal = ev::getProperty(v, "wordSpacing");
        o.spacing.word = ev::isNumber(wsVal) ? static_cast<float>(ev::toDouble(wsVal)) : 0.0f;
    } else {
        o.ref.size = 16.0f;
    }
    o.ref.family = o.family;
    return o;
}

static const render::ShapedRun* shapeArgs(std::span<const Value> a, std::string& textOut, Opts& optsOut) {
    auto* eng = hostEngine();
    render::Renderer* r = eng ? eng->renderer() : nullptr;
    if (!r || a.empty()) return nullptr;
    textOut = ev::toUtf8(a[0]);
    optsOut = readOpts(a.size() > 1 ? a[1] : ev::undefined());
    optsOut.ref.family = optsOut.family;
    return r->shapeText(textOut, optsOut.ref, optsOut.spacing.letter != 0.0f);
}

} // namespace

Value makeBroTextValue() {
    ObjectBuilder t;

    t.accessor("bidiAvailable", [](Value, std::span<const Value>) -> Value {
        return ev::fromBool(render::bidi::available());
    }, nullptr);

    t.def("shape", 2, [](Value, std::span<const Value> a) -> Value {
        std::string text;
        Opts opts;
        const render::ShapedRun* run = shapeArgs(a, text, opts);
        if (!run) return ev::null();

        ObjectBuilder out;
        out.set("text", ev::fromUtf8(text));
        out.set("glyphCount", ev::fromDouble(static_cast<double>(run->glyphCount())));
        out.set("width", ev::fromDouble(run->width(opts.spacing)));

        auto list = run->clusterList(opts.spacing);
        Value clusters = hostArrayOf(list.size(), [&list](size_t i) -> Value {
            const auto& c = list[i];
            ObjectBuilder e;
            e.set("start", ev::fromDouble(static_cast<double>(c.byteStart)));
            e.set("end", ev::fromDouble(static_cast<double>(c.byteEnd)));
            e.set("x", ev::fromDouble(c.x));
            e.set("advance", ev::fromDouble(c.advance));
            e.set("glyphs", ev::fromDouble(static_cast<double>(c.glyphCount)));
            e.set("rtl", ev::fromBool(c.rtl));
            return e.get();
        });
        out.set("clusters", clusters);
        return out.get();
    });

    t.def("byteOffsetToX", 3, [](Value, std::span<const Value> a) -> Value {
        std::string text;
        Opts opts;
        const render::ShapedRun* run = shapeArgs(a, text, opts);
        if (!run) return ev::null();
        int32_t off = a.size() > 2 ? static_cast<int32_t>(ev::toDouble(a[2])) : 0;
        auto pos = run->byteOffsetToX(static_cast<size_t>(off < 0 ? 0 : off), opts.spacing);
        ObjectBuilder out;
        out.set("x", ev::fromDouble(pos.primary.x));
        out.set("isLeadingEdge", ev::fromBool(pos.primary.isLeadingEdge));
        if (pos.hasSecondary) {
            ObjectBuilder sec;
            sec.set("x", ev::fromDouble(pos.secondary.x));
            sec.set("isLeadingEdge", ev::fromBool(pos.secondary.isLeadingEdge));
            out.set("secondary", sec.get());
        }
        return out.get();
    });

    t.def("xToByteOffset", 3, [](Value, std::span<const Value> a) -> Value {
        std::string text;
        Opts opts;
        const render::ShapedRun* run = shapeArgs(a, text, opts);
        if (!run) return ev::null();
        double x = a.size() > 2 ? ev::toDouble(a[2]) : 0.0;
        return ev::fromDouble(static_cast<double>(run->xToByteOffset(static_cast<float>(x), opts.spacing)));
    });

    t.def("clusterRange", 3, [](Value, std::span<const Value> a) -> Value {
        std::string text;
        Opts opts;
        const render::ShapedRun* run = shapeArgs(a, text, opts);
        if (!run) return ev::null();
        int32_t off = a.size() > 2 ? static_cast<int32_t>(ev::toDouble(a[2])) : 0;
        auto span = run->clusterRange(static_cast<size_t>(off < 0 ? 0 : off));
        ObjectBuilder out;
        out.set("start", ev::fromDouble(static_cast<double>(span.byteStart)));
        out.set("end", ev::fromDouble(static_cast<double>(span.byteEnd)));
        return out.get();
    });

    t.def("cacheStats", 0, [](Value, std::span<const Value>) -> Value {
        auto* eng = hostEngine();
        render::Renderer* r = eng ? eng->renderer() : nullptr;
        render::TextShapingEngine* te = r ? r->textEngine() : nullptr;
        ObjectBuilder out;
        out.set("hits", ev::fromDouble(te ? static_cast<double>(te->hits()) : 0.0));
        out.set("misses", ev::fromDouble(te ? static_cast<double>(te->misses()) : 0.0));
        return out.get();
    });

    t.def("bidi", 3, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || ev::isUndefined(a[0])) return ev::null();
        std::string text = ev::toUtf8(a[0]);
        render::bidi::BaseDirection base = render::bidi::BaseDirection::Auto;
        if (a.size() > 1 && ev::isString(a[1])) {
            std::string b = ev::toUtf8(a[1]);
            if (b == "ltr") base = render::bidi::BaseDirection::LTR;
            else if (b == "rtl") base = render::bidi::BaseDirection::RTL;
        }
        render::bidi::Override ov = render::bidi::Override::Normal;
        if (a.size() > 2 && ev::toBool(a[2])) ov = render::bidi::Override::Override;

        const render::bidi::Paragraph para = render::bidi::resolveParagraph(text, base, ov);
        ObjectBuilder out;
        out.set("paragraphLevel", ev::fromDouble(para.paragraphLevel));
        out.set("uniform", ev::fromBool(para.uniform));

        std::vector<int> codepointLevels;
        for (size_t i = 0; i < text.size(); ++i) {
            if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80) continue;
            codepointLevels.push_back(para.levels[i]);
        }
        Value levels = hostArrayOf(codepointLevels.size(), [&codepointLevels](size_t i) -> Value {
            return ev::fromDouble(codepointLevels[i]);
        });
        out.set("levels", levels);

        const auto& pruns = para.runs();
        Value runs = hostArrayOf(pruns.size(), [&pruns](size_t i) -> Value {
            const auto& r = pruns[i];
            ObjectBuilder e;
            e.set("start", ev::fromDouble(static_cast<double>(r.start)));
            e.set("end", ev::fromDouble(static_cast<double>(r.end)));
            e.set("level", ev::fromDouble(r.level));
            return e.get();
        });
        out.set("runs", runs);
        return out.get();
    });

    t.def("bidiReorder", 1, [](Value, std::span<const Value> a) -> Value {
        if (a.empty() || !hostIsArray(a[0])) return ev::null();
        const Value& arr = a[0];  // the rooted slot, current across the reads
        uint32_t n = static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr, "length")));
        std::vector<render::bidi::Level> levels;
        levels.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            int32_t lv = static_cast<int32_t>(ev::toDouble(ev::getElement(arr, i)));
            levels.push_back(static_cast<render::bidi::Level>(lv < 0 ? 0 : lv));
        }
        const std::vector<int32_t> order = render::bidi::reorderVisual(levels);
        return hostArrayOf(order.size(), [&order](size_t i) -> Value {
            return ev::fromDouble(order[i]);
        });
    });

    return t.get();
}

} // namespace bro::bronze_host
