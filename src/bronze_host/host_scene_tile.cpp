#if BRO_WITH_3D

#include "bronze_host/host_scene_internal.h"
#include "scene/tile_world.h"
#include "scene/scene_graph.h"
#include "util/asset_path.h"
#include "broimage/decode.h"
#include "tile/serialize.h"
#include "tile/coord.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::bronze_host {

HostClass g_tileWorldClass;

namespace {

static constexpr int64_t kMaxTileCells = 16 * 1024 * 1024;
static constexpr int     kMaxTileDim   = 65536;

static void readFloatArray(Value v, std::vector<float>& out) {
    if (ev::isUndefined(v) || ev::isNull(v)) return;
    auto info = ev::typedArrayInfo(v);
    if (info && info.data && info.byteLength > 0) {
        if (info.elementKind == ev::elements::Float32) {
            const float* fp = reinterpret_cast<const float*>(info.data);
            out.assign(fp, fp + info.elementCount);
            return;
        }
    }
    auto abInfo = ev::arrayBufferInfo(v);
    if (abInfo && abInfo.data && abInfo.byteLength > 0) {
        size_t count = abInfo.byteLength / sizeof(float);
        const float* fp = reinterpret_cast<const float*>(abInfo.data);
        out.assign(fp, fp + count);
        return;
    }
    if (ev::isObject(v)) {
        Value lenVal = ev::getProperty(v, "length");
        if (ev::isNumber(lenVal)) {
            uint32_t len = static_cast<uint32_t>(ev::toDouble(lenVal));
            out.resize(len);
            for (uint32_t i = 0; i < len; ++i) {
                Value el = ev::getElement(v, i);
                out[i] = ev::isNumber(el) ? static_cast<float>(ev::toDouble(el)) : 0.0f;
            }
        }
    }
}

static bool readVec3Arg(Value v, bromath::Vec3& out) {
    if (!ev::isObject(v)) return false;
    Value x = ev::getElement(v, 0);
    Value y = ev::getElement(v, 1);
    Value z = ev::getElement(v, 2);
    if (ev::isNumber(x) && ev::isNumber(y) && ev::isNumber(z)) {
        out = {static_cast<float>(ev::toDouble(x)),
               static_cast<float>(ev::toDouble(y)),
               static_cast<float>(ev::toDouble(z))};
        return true;
    }
    Value px = ev::getProperty(v, "x");
    Value py = ev::getProperty(v, "y");
    Value pz = ev::getProperty(v, "z");
    if (ev::isNumber(px) && ev::isNumber(py) && ev::isNumber(pz)) {
        out = {static_cast<float>(ev::toDouble(px)),
               static_cast<float>(ev::toDouble(py)),
               static_cast<float>(ev::toDouble(pz))};
        return true;
    }
    return false;
}

static bool validateTileConfig(const scene::TileWorldConfig& cfg) {
    if (cfg.width < 1 || cfg.height < 1) {
        ev::throwRangeError("TileWorld: width and height must be >= 1 (got " +
                            std::to_string(cfg.width) + "x" + std::to_string(cfg.height) + ")");
        return false;
    }
    if (cfg.width > kMaxTileDim || cfg.height > kMaxTileDim) {
        ev::throwRangeError("TileWorld: width/height must be <= " + std::to_string(kMaxTileDim) +
                            " (got " + std::to_string(cfg.width) + "x" + std::to_string(cfg.height) + ")");
        return false;
    }
    const int64_t cells = static_cast<int64_t>(cfg.width) * static_cast<int64_t>(cfg.height);
    if (cells > kMaxTileCells) {
        ev::throwRangeError("TileWorld: " + std::to_string(cfg.width) + "x" +
                            std::to_string(cfg.height) + " is " + std::to_string(cells) +
                            " cells, over the " + std::to_string(kMaxTileCells) + "-cell limit");
        return false;
    }
    if (cfg.chunkSize < 1) {
        ev::throwRangeError("TileWorld: chunkSize must be >= 1 (got " + std::to_string(cfg.chunkSize) + ")");
        return false;
    }
    return true;
}

static scene::TileWorldConfig parseTileConfig(Value opts) {
    scene::TileWorldConfig cfg;
    if (!ev::isObject(opts)) return cfg;

    Value wVal = ev::getProperty(opts, "width");
    if (ev::isNumber(wVal)) cfg.width = static_cast<int>(ev::toDouble(wVal));

    Value hVal = ev::getProperty(opts, "height");
    if (ev::isNumber(hVal)) cfg.height = static_cast<int>(ev::toDouble(hVal));

    Value csVal = ev::getProperty(opts, "cellSize");
    if (ev::isNumber(csVal)) cfg.cellSize = static_cast<float>(ev::toDouble(csVal));

    Value hsVal = ev::getProperty(opts, "heightStep");
    if (ev::isNumber(hsVal)) cfg.heightStep = static_cast<float>(ev::toDouble(hsVal));

    Value chkVal = ev::getProperty(opts, "chunkSize");
    if (ev::isNumber(chkVal)) cfg.chunkSize = static_cast<int>(ev::toDouble(chkVal));

    Value blVal = ev::getProperty(opts, "baseLevel");
    if (ev::isNumber(blVal)) cfg.baseLevel = static_cast<int>(ev::toDouble(blVal));

    Value aoVal = ev::getProperty(opts, "aoStrength");
    if (ev::isNumber(aoVal)) cfg.aoStrength = static_cast<float>(ev::toDouble(aoVal));

    Value topo = ev::getProperty(opts, "topology");
    if (ev::isString(topo)) {
        std::string s = ev::toUtf8(topo);
        if (s == "hex") cfg.topology = tile::Topology::Hex;
    }

    Value layers = ev::getProperty(opts, "layers");
    if (ev::isObject(layers)) {
        Value lenVal = ev::getProperty(layers, "length");
        if (ev::isNumber(lenVal)) {
            int32_t len = static_cast<int32_t>(ev::toDouble(lenVal));
            std::vector<std::string> names;
            for (int32_t i = 0; i < len; ++i) {
                Value el = ev::getElement(layers, i);
                if (ev::isString(el)) names.emplace_back(ev::toUtf8(el));
            }
            if (!names.empty()) cfg.layers = std::move(names);
        }
    }

    Value orig = ev::getProperty(opts, "origin");
    if (ev::isObject(orig)) {
        Value ox = ev::getElement(orig, 0);
        Value oy = ev::getElement(orig, 1);
        Value oz = ev::getElement(orig, 2);
        double vx = ev::isNumber(ox) ? ev::toDouble(ox) : 0;
        double vy = ev::isNumber(oy) ? ev::toDouble(oy) : 0;
        double vz = ev::isNumber(oz) ? ev::toDouble(oz) : 0;
        cfg.origin = {static_cast<float>(vx), static_cast<float>(vy), static_cast<float>(vz)};
    }

    Value pal = ev::getProperty(opts, "palette");
    readFloatArray(pal, cfg.palette);

    Value acVal = ev::getProperty(opts, "atlasColumns");
    if (ev::isNumber(acVal)) cfg.atlasColumns = static_cast<int>(ev::toDouble(acVal));

    Value arVal = ev::getProperty(opts, "atlasRows");
    if (ev::isNumber(arVal)) cfg.atlasRows = static_cast<int>(ev::toDouble(arVal));

    Value ccVal = ev::getProperty(opts, "cliffCell");
    if (ev::isNumber(ccVal)) cfg.cliffCell = static_cast<int>(ev::toDouble(ccVal));

    Value aiVal = ev::getProperty(opts, "atlasInset");
    if (ev::isNumber(aiVal)) cfg.atlasInset = static_cast<float>(ev::toDouble(aiVal));

    Value atlas = ev::getProperty(opts, "atlas");
    if (ev::isString(atlas)) {
        std::string s = ev::toUtf8(atlas);
        broimage::Image img;
        std::string err;
        if (broimage::decode_file(util::resolveAssetPath(s), img, &err) &&
            img.width > 0 && img.height > 0) {
            cfg.atlasWidth = img.width;
            cfg.atlasHeight = img.height;
            cfg.atlasPixels = std::move(img.pixels);
        }
    } else {
        Value px = ev::getProperty(opts, "atlasPixels");
        auto info = ev::typedArrayInfo(px);
        if (info && info.data && info.byteLength > 0) {
            Value aw = ev::getProperty(opts, "atlasWidth");
            Value ah = ev::getProperty(opts, "atlasHeight");
            cfg.atlasWidth = ev::isNumber(aw) ? static_cast<int>(ev::toDouble(aw)) : 0;
            cfg.atlasHeight = ev::isNumber(ah) ? static_cast<int>(ev::toDouble(ah)) : 0;
            cfg.atlasPixels.assign(info.data, info.data + info.byteLength);
        }
    }

    Value ta = ev::getProperty(opts, "tileAtlas");
    if (ev::isObject(ta)) {
        Value lenVal = ev::getProperty(ta, "length");
        if (ev::isNumber(lenVal)) {
            int32_t len = static_cast<int32_t>(ev::toDouble(lenVal));
            cfg.tileAtlas.resize(len);
            for (int32_t i = 0; i < len; ++i) {
                Value el = ev::getElement(ta, i);
                cfg.tileAtlas[i] = ev::isNumber(el) ? static_cast<int>(ev::toDouble(el)) : 0;
            }
        }
    }

    Value autos = ev::getProperty(opts, "autotiles");
    if (ev::isObject(autos)) {
        Value lenVal = ev::getProperty(autos, "length");
        if (ev::isNumber(lenVal)) {
            int32_t len = static_cast<int32_t>(ev::toDouble(lenVal));
            for (int32_t i = 0; i < len; ++i) {
                Value e = ev::getElement(autos, i);
                if (!ev::isObject(e)) continue;
                scene::TileWorldConfig::AutotileRule rule;
                Value idVal = ev::getProperty(e, "id");
                if (ev::isNumber(idVal)) rule.id = static_cast<uint16_t>(ev::toDouble(idVal));
                Value lVal = ev::getProperty(e, "layer");
                if (ev::isNumber(lVal)) rule.layer = static_cast<int>(ev::toDouble(lVal));

                Value mode = ev::getProperty(e, "mode");
                if (ev::isString(mode)) {
                    std::string m = ev::toUtf8(mode);
                    if (m == "edge") rule.mode = scene::TileWorldConfig::AutotileMode::Edge;
                    else if (m == "wang") rule.mode = scene::TileWorldConfig::AutotileMode::Wang;
                    else rule.mode = scene::TileWorldConfig::AutotileMode::Blob47;
                }

                Value famv = ev::getProperty(e, "family");
                if (ev::isString(famv)) {
                    std::string s = ev::toUtf8(famv);
                    if (s == "nonEmpty") rule.family = scene::TileWorldConfig::AutotileFamily::NonEmpty;
                }

                Value cells = ev::getProperty(e, "cells");
                if (ev::isObject(cells)) {
                    Value cl = ev::getProperty(cells, "length");
                    if (ev::isNumber(cl)) {
                        int32_t clen = static_cast<int32_t>(ev::toDouble(cl));
                        rule.cells.resize(clen);
                        for (int32_t k = 0; k < clen; ++k) {
                            Value el = ev::getElement(cells, k);
                            rule.cells[k] = ev::isNumber(el) ? static_cast<int>(ev::toDouble(el)) : 0;
                        }
                    }
                }
                cfg.autotiles.push_back(std::move(rule));
            }
        }
    }

    Value ovs = ev::getProperty(opts, "overlays");
    if (ev::isObject(ovs)) {
        Value lenVal = ev::getProperty(ovs, "length");
        if (ev::isNumber(lenVal)) {
            int32_t len = static_cast<int32_t>(ev::toDouble(lenVal));
            cfg.overlays.resize(len);
            for (int32_t i = 0; i < len; ++i) {
                Value e = ev::getElement(ovs, i);
                if (ev::isObject(e)) {
                    Value op = ev::getProperty(e, "opacity");
                    if (ev::isNumber(op)) cfg.overlays[i].opacity = static_cast<float>(ev::toDouble(op));
                    Value ac = ev::getProperty(e, "alphaCutoff");
                    if (ev::isNumber(ac)) cfg.overlays[i].alphaCutoff = static_cast<float>(ev::toDouble(ac));
                }
            }
        }
    }

    Value anims = ev::getProperty(opts, "animations");
    if (ev::isObject(anims)) {
        Value lenVal = ev::getProperty(anims, "length");
        if (ev::isNumber(lenVal)) {
            int32_t len = static_cast<int32_t>(ev::toDouble(lenVal));
            for (int32_t i = 0; i < len; ++i) {
                Value e = ev::getElement(anims, i);
                if (!ev::isObject(e)) continue;
                scene::TileWorldConfig::TileAnimation an;
                Value idVal = ev::getProperty(e, "id");
                if (ev::isNumber(idVal)) an.id = static_cast<uint16_t>(ev::toDouble(idVal));
                Value fpsVal = ev::getProperty(e, "fps");
                if (ev::isNumber(fpsVal)) an.fps = static_cast<float>(ev::toDouble(fpsVal));
                else an.fps = 4.0f;

                Value fr = ev::getProperty(e, "frames");
                if (ev::isObject(fr)) {
                    Value fl = ev::getProperty(fr, "length");
                    if (ev::isNumber(fl)) {
                        int32_t flen = static_cast<int32_t>(ev::toDouble(fl));
                        an.frames.resize(flen);
                        for (int32_t k = 0; k < flen; ++k) {
                            Value el = ev::getElement(fr, k);
                            an.frames[k] = ev::isNumber(el) ? static_cast<int>(ev::toDouble(el)) : 0;
                        }
                    }
                }
                cfg.animations.push_back(std::move(an));
            }
        }
    }

    return cfg;
}

} // namespace

void ensureTileWorldClassInstalled() {
    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;
    g_tileWorldClass.install("TileWorld", 0, nullptr, [](ObjectBuilder& b) {
        b.def("setTile", 3, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 3) return ev::undefined();
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            uint16_t tileId = ev::isNumber(a[2]) ? static_cast<uint16_t>(ev::toDouble(a[2])) : 0;
            int layer = (a.size() > 3 && ev::isNumber(a[3])) ? static_cast<int>(ev::toDouble(a[3])) : 0;
            w->setTile(x, y, tileId, layer);
            return ev::undefined();
        });

        b.def("getTile", 2, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 2) return ev::fromDouble(0);
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int layer = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            return ev::fromDouble(w->tile(x, y, layer));
        });

        b.def("fillTile", 5, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 5) return ev::undefined();
            int x0 = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y0 = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int x1 = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            int y1 = ev::isNumber(a[3]) ? static_cast<int>(ev::toDouble(a[3])) : 0;
            uint16_t tileId = ev::isNumber(a[4]) ? static_cast<uint16_t>(ev::toDouble(a[4])) : 0;
            int layer = (a.size() > 5 && ev::isNumber(a[5])) ? static_cast<int>(ev::toDouble(a[5])) : 0;
            w->fillTile(x0, y0, x1, y1, tileId, layer);
            return ev::undefined();
        });

        b.def("setElevation", 3, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 3) return ev::undefined();
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int level = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            w->setElevation(x, y, level);
            return ev::undefined();
        });

        b.def("getElevation", 2, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 2) return ev::fromDouble(0);
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            return ev::fromDouble(w->elevation(x, y));
        });

        b.def("fillElevation", 5, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 5) return ev::undefined();
            int x0 = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y0 = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int x1 = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            int y1 = ev::isNumber(a[3]) ? static_cast<int>(ev::toDouble(a[3])) : 0;
            int level = ev::isNumber(a[4]) ? static_cast<int>(ev::toDouble(a[4])) : 0;
            w->fillElevation(x0, y0, x1, y1, level);
            return ev::undefined();
        });

        b.def("setTint", 5, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 5) return ev::undefined();
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            float r = ev::isNumber(a[2]) ? static_cast<float>(ev::toDouble(a[2])) : 1.0f;
            float g = ev::isNumber(a[3]) ? static_cast<float>(ev::toDouble(a[3])) : 1.0f;
            float bv = ev::isNumber(a[4]) ? static_cast<float>(ev::toDouble(a[4])) : 1.0f;
            float alpha = (a.size() > 5 && ev::isNumber(a[5])) ? static_cast<float>(ev::toDouble(a[5])) : 1.0f;
            w->setTint(x, y, r, g, bv, alpha);
            return ev::undefined();
        });

        b.def("fillTint", 7, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 7) return ev::undefined();
            int x0 = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y0 = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int x1 = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            int y1 = ev::isNumber(a[3]) ? static_cast<int>(ev::toDouble(a[3])) : 0;
            float r = ev::isNumber(a[4]) ? static_cast<float>(ev::toDouble(a[4])) : 1.0f;
            float g = ev::isNumber(a[5]) ? static_cast<float>(ev::toDouble(a[5])) : 1.0f;
            float bv = ev::isNumber(a[6]) ? static_cast<float>(ev::toDouble(a[6])) : 1.0f;
            float alpha = (a.size() > 7 && ev::isNumber(a[7])) ? static_cast<float>(ev::toDouble(a[7])) : 1.0f;
            w->fillTint(x0, y0, x1, y1, r, g, bv, alpha);
            return ev::undefined();
        });

        b.def("getTint", 2, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            uint32_t packed = 0xFFFFFFFFu;
            if (w && a.size() >= 2) {
                int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
                int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
                packed = w->tintAt(x, y);
            }
            ObjectBuilder ob;
            ob.set("r", ev::fromDouble(((packed >> 24) & 0xFF) / 255.0));
            ob.set("g", ev::fromDouble(((packed >> 16) & 0xFF) / 255.0));
            ob.set("b", ev::fromDouble(((packed >>  8) & 0xFF) / 255.0));
            ob.set("a", ev::fromDouble(( packed        & 0xFF) / 255.0));
            return ob.get();
        });

        b.def("setShade", 3, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 3) return ev::undefined();
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            float shade = ev::isNumber(a[2]) ? static_cast<float>(ev::toDouble(a[2])) : 1.0f;
            w->setShade(x, y, shade);
            return ev::undefined();
        });

        b.def("fillShade", 5, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 5) return ev::undefined();
            int x0 = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y0 = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            int x1 = ev::isNumber(a[2]) ? static_cast<int>(ev::toDouble(a[2])) : 0;
            int y1 = ev::isNumber(a[3]) ? static_cast<int>(ev::toDouble(a[3])) : 0;
            float shade = ev::isNumber(a[4]) ? static_cast<float>(ev::toDouble(a[4])) : 1.0f;
            w->fillShade(x0, y0, x1, y1, shade);
            return ev::undefined();
        });

        b.def("setShadeMap", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w || a.empty()) return ev::undefined();
            auto info = ev::typedArrayInfo(a[0]);
            if (info && info.data && info.byteLength > 0) {
                if (info.bytesPerElement == 4) {
                    w->setShadeMap(reinterpret_cast<const float*>(info.data), info.byteLength / 4);
                } else if (info.bytesPerElement == 1) {
                    w->setShadeMap(info.data, info.byteLength);
                }
                return ev::undefined();
            }
            std::vector<float> values;
            readFloatArray(a[0], values);
            w->setShadeMap(values.data(), values.size());
            return ev::undefined();
        });

        b.def("getShade", 2, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 2) return ev::fromDouble(1.0);
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            return ev::fromDouble(w->shadeAt(x, y));
        });

        b.def("setFlag", 4, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 4) return ev::undefined();
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            uint32_t bit = ev::isNumber(a[2]) ? static_cast<uint32_t>(ev::toDouble(a[2])) : 0u;
            bool on = ev::toBool(a[3]);
            w->setFlag(x, y, bit, on);
            return ev::undefined();
        });

        b.def("hasFlag", 3, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 3) return ev::fromBool(false);
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            uint32_t bit = ev::isNumber(a[2]) ? static_cast<uint32_t>(ev::toDouble(a[2])) : 0u;
            return ev::fromBool(w->hasFlag(x, y, bit));
        });

        b.def("rebuild", 0, [](Value self_, std::span<const Value>) {
            auto* w = tileWorldOf(self_);
            if (w) w->rebuildDirty();
            return ev::undefined();
        });

        b.def("rebuildAll", 0, [](Value self_, std::span<const Value>) {
            auto* w = tileWorldOf(self_);
            if (w) w->rebuildAll();
            return ev::undefined();
        });

        b.def("setOrigin", 3, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 3) return ev::undefined();
            float x = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float y = ev::isNumber(a[1]) ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float z = ev::isNumber(a[2]) ? static_cast<float>(ev::toDouble(a[2])) : 0.0f;
            w->setOrigin(x, y, z);
            return ev::undefined();
        });

        b.def("advance", 1, [](Value self_, std::span<const Value> a) {
            auto* w = tileWorldOf(self_);
            if (!w || a.empty()) return ev::fromBool(false);
            double dt = ev::isNumber(a[0]) ? ev::toDouble(a[0]) : 0.0;
            return ev::fromBool(w->advance(dt));
        });

        b.def("worldToCell", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 2) return ev::null();
            float wx = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float wz = ev::isNumber(a[1]) ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            int cx = 0, cy = 0;
            if (!w->worldToCell(wx, wz, cx, cy)) return ev::null();
            ObjectBuilder ob;
            ob.set("x", ev::fromDouble(cx));
            ob.set("y", ev::fromDouble(cy));
            return ob.get();
        });

        b.def("cellCenterWorldXZ", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 2) return ev::null();
            int cx = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int cy = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            float px = 0, pz = 0;
            w->cellCenterWorldXZ(cx, cy, px, pz);
            ObjectBuilder ob;
            ob.set("x", ev::fromDouble(px));
            ob.set("z", ev::fromDouble(pz));
            return ob.get();
        });

        b.def("worldBounds", 0, [](Value self_, std::span<const Value>) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w) return ev::null();
            float minX = 0, minZ = 0, maxX = 0, maxZ = 0;
            w->worldBounds(minX, minZ, maxX, maxZ);
            ObjectBuilder ob;
            ob.set("minX", ev::fromDouble(minX));
            ob.set("minZ", ev::fromDouble(minZ));
            ob.set("maxX", ev::fromDouble(maxX));
            ob.set("maxZ", ev::fromDouble(maxZ));
            return ob.get();
        });

        b.def("raycastCell", 3, [](Value self_, std::span<const Value> a) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 2) return ev::null();
            bromath::Vec3 o, d;
            if (!readVec3Arg(a[0], o) || !readVec3Arg(a[1], d)) return ev::null();
            float maxDist = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<float>(ev::toDouble(a[2])) : 1.0e6f;
            auto hit = w->raycastCell(o, d, maxDist);
            if (!hit.hit) return ev::null();

            ObjectBuilder ob;
            ob.set("x", ev::fromDouble(hit.x));
            ob.set("y", ev::fromDouble(hit.y));
            ob.set("distance", ev::fromDouble(hit.distance));
            ob.set("side", ev::fromBool(hit.side));
            ob.set("cell", hostArrayOf(2, [&](size_t i) {
                return ev::fromDouble(i == 0 ? hit.x : hit.y);
            }));
            ob.set("point", hostArrayOf(3, [&](size_t i) {
                return ev::fromDouble(hit.point[i]);
            }));
            return ob.get();
        });

        b.def("sampleHeight", 2, [](Value self_, std::span<const Value> a) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 2) return ev::null();
            float wx = ev::isNumber(a[0]) ? static_cast<float>(ev::toDouble(a[0])) : 0.0f;
            float wz = ev::isNumber(a[1]) ? static_cast<float>(ev::toDouble(a[1])) : 0.0f;
            float y = 0;
            if (!w->sampleHeight(wx, wz, y)) return ev::null();
            return ev::fromDouble(y);
        });

        b.def("isWalkable", 3, [](Value self_, std::span<const Value> a) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w || a.size() < 2) return ev::fromBool(false);
            int x = ev::isNumber(a[0]) ? static_cast<int>(ev::toDouble(a[0])) : 0;
            int y = ev::isNumber(a[1]) ? static_cast<int>(ev::toDouble(a[1])) : 0;
            uint32_t mask = (a.size() > 2 && ev::isNumber(a[2])) ? static_cast<uint32_t>(ev::toDouble(a[2])) : 0u;
            return ev::fromBool(w->isWalkable(x, y, mask));
        });

        b.def("configure", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w || a.empty() || !ev::isObject(a[0])) return ev::undefined();
            scene::TileWorldConfig cfg = parseTileConfig(a[0]);
            if (!validateTileConfig(cfg)) return ev::undefined();
            w->configure(std::move(cfg));
            return ev::undefined();
        });

        b.def("save", 0, [](Value self_, std::span<const Value>) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w || !w->grid()) return ev::null();
            std::vector<uint8_t> bytes = tile::serialize(*w->grid());
            ev::Persistent view(ev::createTypedArray(ev::elements::Uint8, static_cast<uint32_t>(bytes.size())));
            ev::fillTypedArray(view.get(), std::span<const uint8_t>(bytes.data(), bytes.size()));
            return view.get();
        });

        b.def("load", 1, [](Value self_, std::span<const Value> a) -> Value {
            auto* w = tileWorldOf(self_);
            if (!w || a.empty()) return ev::fromBool(false);
            std::vector<uint8_t> bytes;
            auto info = ev::typedArrayInfo(a[0]);
            if (info && info.data && info.byteLength > 0) {
                bytes.assign(info.data, info.data + info.byteLength);
            } else {
                auto abInfo = ev::arrayBufferInfo(a[0]);
                if (abInfo && abInfo.data && abInfo.byteLength > 0) {
                    bytes.assign(abInfo.data, abInfo.data + abInfo.byteLength);
                }
            }
            auto grid = tile::deserialize(bytes);
            if (!grid) return ev::fromBool(false);
            w->loadGrid(std::move(*grid));
            return ev::fromBool(true);
        });

        b.def("destroy", 0, [](Value self_, std::span<const Value>) {
            auto* c = tileWorldCellOf(self_);
            if (c && c->world) {
                c->world->clear();
                c->world.reset();
            }
            return ev::undefined();
        });

        b.accessor("width", [](Value self_, std::span<const Value>) {
            auto* w = tileWorldOf(self_);
            return ev::fromDouble(w ? w->width() : 0);
        }, nullptr);

        b.accessor("height", [](Value self_, std::span<const Value>) {
            auto* w = tileWorldOf(self_);
            return ev::fromDouble(w ? w->height() : 0);
        }, nullptr);

        b.accessor("chunkCount", [](Value self_, std::span<const Value>) {
            auto* w = tileWorldOf(self_);
            return ev::fromDouble(w ? w->chunkCount() : 0);
        }, nullptr);

        b.accessor("vertexCount", [](Value self_, std::span<const Value>) {
            auto* w = tileWorldOf(self_);
            return ev::fromDouble(w ? w->totalVertices() : 0);
        }, nullptr);

        b.accessor("triangleCount", [](Value self_, std::span<const Value>) {
            auto* w = tileWorldOf(self_);
            return ev::fromDouble(w ? w->totalTriangles() : 0);
        }, nullptr);

        installTileWorldExtra(b);
    });
}

Value wrapTileWorld(std::unique_ptr<scene::TileWorld> world, scene::SceneGraph* graph) {
    if (!world) return ev::null();
    ensureTileWorldClassInstalled();
    auto* cell = new HostTileWorldCell();
    cell->token = graph ? graph->livenessToken() : std::weak_ptr<scene::SceneGraph::LivenessToken>();
    cell->world = std::move(world);
    return g_tileWorldClass.make(cell, [](void* p) {
        auto* c = static_cast<HostTileWorldCell*>(p);
        if (c) {
            if (c->world) {
                c->world->clear();
                c->world.reset();
            }
            delete c;
        }
    });
}

void installSceneTileWorld(ObjectBuilder& bGraph) {
    bGraph.def("createTileWorld", 1, [](Value self_, std::span<const Value> a) -> Value {
        auto* g = sceneGraphOf(self_);
        if (!g) return ev::throwTypeError("createTileWorld: no scene graph");
        Value opts = a.empty() ? ev::undefined() : a[0];
        scene::TileWorldConfig cfg = parseTileConfig(opts);
        if (!validateTileConfig(cfg)) return ev::undefined();
        auto world = std::make_unique<scene::TileWorld>(*g);
        world->configure(std::move(cfg));
        return wrapTileWorld(std::move(world), g);
    });
}

} // namespace bro::bronze_host

#endif // BRO_WITH_3D
