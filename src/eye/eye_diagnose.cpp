#include "eye/eye_diagnose.h"
#include "eye/eye_draw_internal.h"

#include "scene/debug_views.h"
#include "scene/nodes/instanced_mesh_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include <broimage/encode.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::eye {
namespace {

inline double srgbLum(const uint8_t* px) {
    return (0.2126 * px[0] + 0.7152 * px[1] + 0.0722 * px[2]) / 255.0;
}

void rgbToHsv(double r, double g, double b, double& hDeg, double& s, double& v) {
    const double mx = std::max({r, g, b});
    const double mn = std::min({r, g, b});
    const double d = mx - mn;
    v = mx;
    s = mx > 1e-6 ? d / mx : 0.0;
    if (d <= 1e-6) {
        hDeg = 0.0;
        return;
    }
    if (mx == r) hDeg = 60.0 * std::fmod((g - b) / d, 6.0);
    else if (mx == g) hDeg = 60.0 * ((b - r) / d + 2.0);
    else hDeg = 60.0 * ((r - g) / d + 4.0);
    if (hDeg < 0.0) hDeg += 360.0;
}

double wrapHueDiff(double a, double b) {
    double d = std::abs(a - b);
    return d > 180.0 ? 360.0 - d : d;
}

bool isGroundOrTerrainName(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (unsigned char c : name) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    return lower.find("ground") != std::string::npos ||
           lower.find("terrain") != std::string::npos ||
           lower == "floor" ||
           lower.find("floor_slab") != std::string::npos ||
           lower.find("floorplate") != std::string::npos ||
           lower.find("bedplate") != std::string::npos;
}

bool isArchitecturalOrUrbanName(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (unsigned char c : name) {
        lower.push_back(static_cast<char>(std::tolower(c)));
    }
    static constexpr const char* kUrbanTokens[] = {
        "bldg", "building", "wall", "door", "window", "glass", "slab", "roof",
        "floor", "stair", "elevator", "parallax", "shell", "facade", "tower",
        "furn_", "table", "chair", "desk", "sofa", "bed", "counter", "shelf",
        "lamp", "road", "street", "sidewalk", "ground", "plaza", "car",
        "vehicle", "traffic", "bridge", "curb", "pillar", "column", "frame",
        "panel", "rail", "deck", "lantern", "porch", "masonry",
    };
    for (const char* tok : kUrbanTokens) {
        if (lower.find(tok) != std::string::npos) return true;
    }
    return false;
}

bool sampleTextureMeanSrgb(const scene::NodeTexture& tex, const float* tint, double& outR, double& outG, double& outB) {
    if (tex.empty() || tex.rgba.size() < static_cast<size_t>(tex.width) * tex.height * 4) return false;
    const int stepX = std::max(1, tex.width / 32);
    const int stepY = std::max(1, tex.height / 32);
    double sr = 0, sg = 0, sb = 0;
    size_t cnt = 0;
    for (int y = 0; y < tex.height; y += stepY) {
        for (int x = 0; x < tex.width; x += stepX) {
            const size_t o = (static_cast<size_t>(y) * tex.width + x) * 4;
            if (tex.rgba[o + 3] < 128) continue;
            const double lr = std::pow(tex.rgba[o + 0] / 255.0, 2.2) * (tint ? tint[0] : 1.0);
            const double lg = std::pow(tex.rgba[o + 1] / 255.0, 2.2) * (tint ? tint[1] : 1.0);
            const double lb = std::pow(tex.rgba[o + 2] / 255.0, 2.2) * (tint ? tint[2] : 1.0);
            sr += std::pow(std::clamp(lr, 0.0, 1.0), 1.0 / 2.2);
            sg += std::pow(std::clamp(lg, 0.0, 1.0), 1.0 / 2.2);
            sb += std::pow(std::clamp(lb, 0.0, 1.0), 1.0 / 2.2);
            ++cnt;
        }
    }
    if (cnt == 0) return false;
    outR = sr / cnt; outG = sg / cnt; outB = sb / cnt;
    return true;
}

struct DiagNodeInfo {
    scene::SceneNode* sn = nullptr;
    bool valid = false;
    bool instanced = false;
    int instanceCount = 1;
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissive = 0.0f;
    bool effectiveUnlit = false;
    bool hasAlbedoMap = false;
    bool hasNormalMap = false;
    bool hasMRMap = false;
    bool castsShadow = true;
    bool hasCustomShader = false;
    bool twoSided = false;
    bool atlasTiled = false;
    int vertexColorMode = 0;
    const scene::NodeTexture* baseColorTex = nullptr;
    const bromesh::MeshData* facetMesh = nullptr;
};

bool hasDrawableInstanced(const scene::InstancedMeshNode* im) {
    if (!im) return false;
    if (im->isTube()) return im->tubeSegCount() > 0;
    if (im->isScatter()) return !im->mesh().empty() && im->scatterSegCount() > 0 && im->scatterInstanceCount() > 0;
    return !im->mesh().empty() && im->instanceCount() > 0;
}

DiagNodeInfo inspectDiagNode(scene::SceneNode* sn) {
    DiagNodeInfo info;
    info.sn = sn;
    if (!sn) return info;
    if (sn->type() == scene::SceneNode::Type::Mesh) {
        const auto* mn = static_cast<const scene::MeshNode*>(sn);
        info.valid = true;
        info.instanced = false;
        info.instanceCount = 1;
        for (int k = 0; k < 4; ++k) info.color[k] = mn->color()[k];
        info.emissive = mn->emissive();
        info.effectiveUnlit = mn->effectiveUnlit();
        info.hasAlbedoMap = mn->hasBaseColorTexture();
        info.hasNormalMap = mn->hasNormalTexture();
        info.hasMRMap = mn->hasMetallicRoughnessTexture();
        info.castsShadow = mn->castsShadow();
        info.hasCustomShader = mn->hasCustomShader();
        info.twoSided = mn->twoSided();
        info.atlasTiled = false;
        info.vertexColorMode = mn->vertexColorMode();
        info.baseColorTex = !mn->baseColorTexture().empty() ? &mn->baseColorTexture() : nullptr;
        const auto& cm = mn->currentMesh();
        info.facetMesh = !cm.empty() ? &cm : nullptr;
    } else if (sn->type() == scene::SceneNode::Type::InstancedMesh) {
        const auto* im = static_cast<const scene::InstancedMeshNode*>(sn);
        info.valid = true;
        info.instanced = true;
        info.instanceCount = im->isTube() ? static_cast<int>(im->tubeSegCount())
                           : im->isScatter() ? static_cast<int>(im->scatterInstanceCount())
                                             : static_cast<int>(im->instanceCount());
        for (int k = 0; k < 4; ++k) info.color[k] = im->color()[k];
        info.emissive = im->emissive();
        info.effectiveUnlit = im->effectiveUnlit();
        info.hasAlbedoMap = im->hasBaseColorTexture();
        info.hasNormalMap = im->hasNormalTexture();
        info.hasMRMap = im->hasMetallicRoughnessTexture();
        info.castsShadow = im->castsShadow();
        info.hasCustomShader = im->hasCustomShader();
        info.twoSided = im->doubleSided();
        info.atlasTiled = (im->atlasCols() > 1 || im->atlasRows() > 1);
        info.vertexColorMode = (!im->renderingBatched()
            ? (im->useVertexColorForDraw() ? 1 : 0)
            : (im->mesh().hasColors() && im->vertexColorTintEnabled() ? 1 : 0));
        info.baseColorTex = !im->baseColorTexture().empty() ? &im->baseColorTexture() : nullptr;
        info.facetMesh = (!im->isTube() && !im->mesh().empty()) ? &im->mesh() : nullptr;
    }
    return info;
}

bool subtreeHasTranslucent(const scene::SceneNode* node) {
    if (!node || !node->renderVisible()) return false;
    if (node->type() == scene::SceneNode::Type::Mesh) {
        const auto* mn = static_cast<const scene::MeshNode*>(node);
        if (!mn->currentMesh().positions.empty() && mn->color()[3] < 0.99f) return true;
    } else if (node->type() == scene::SceneNode::Type::InstancedMesh) {
        const auto* im = static_cast<const scene::InstancedMeshNode*>(node);
        // Matches SceneMeshDrawer::fillMaterial (translucent = node.color()[3] < 1.0f; per-instance rec[15] is atlas cell index)
        if (hasDrawableInstanced(im) && im->color()[3] < 0.99f) {
            return true;
        }
    }
    for (const scene::SceneNode* ch : node->children()) {
        if (subtreeHasTranslucent(ch)) return true;
    }
    return false;
}

struct Accum {
    uint32_t id = 0;
    std::string name;
    DiagNodeInfo info;
    int minX = 1e9, minY = 1e9, maxX = -1, maxY = -1;
    int count = 0;
    int lumHist[256] = {};
    double alR = 0, alG = 0, alB = 0, alLum = 0;
    double rSum = 0, rSq = 0;
    double nSum[3] = {0, 0, 0}, nSq[3] = {0, 0, 0};
    double gradSum = 0;
    double axisGrad = 0, totalGrad = 0;
    int gradCnt = 0;
    int clippedCount = 0;
};

double histQuantile(const int hist[256], int count, double q) {
    if (count <= 0) return 0.0;
    const int target = static_cast<int>(std::clamp(q, 0.0, 1.0) * (count - 1));
    int acc = 0;
    for (int i = 0; i < 256; ++i) {
        acc += hist[i];
        if (acc > target) return i / 255.0;
    }
    return 1.0;
}

struct CropBox {
    std::string label;
    bool isError = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

void computeCurvedFacetStats(const bromesh::MeshData& md, int& outCurvedEdges, double& outMedianDeg) {
    outCurvedEdges = 0;
    outMedianDeg = 0.0;
    const size_t vc = md.vertexCount(), tc = md.triangleCount();
    if (vc == 0 || tc == 0 || tc > 16000) return;

    struct QKey {
        int64_t x, y, z;
        bool operator==(const QKey& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct QHash {
        size_t operator()(const QKey& k) const {
            uint64_t h = static_cast<uint64_t>(k.x) * 73856093ULL ^
                         static_cast<uint64_t>(k.y) * 19349663ULL ^
                         static_cast<uint64_t>(k.z) * 83492791ULL;
            return static_cast<size_t>(h ^ (h >> 30));
        }
    };
    std::vector<uint32_t> weld(vc);
    std::unordered_map<QKey, uint32_t, QHash> qmap;
    qmap.reserve(vc);
    uint32_t nextW = 0;
    for (size_t i = 0; i < vc; ++i) {
        const QKey k{
            static_cast<int64_t>(std::llround(md.positions[i * 3 + 0] * 10000.0)),
            static_cast<int64_t>(std::llround(md.positions[i * 3 + 1] * 10000.0)),
            static_cast<int64_t>(std::llround(md.positions[i * 3 + 2] * 10000.0)),
        };
        auto [it, ins] = qmap.emplace(k, nextW);
        if (ins) ++nextW;
        weld[i] = it->second;
    }

    struct EdgeNorms {
        double nx = 0, ny = 0, nz = 0;
        int count = 0;
    };
    std::unordered_map<uint64_t, EdgeNorms> edgeMap;
    edgeMap.reserve(tc * 2);
    std::vector<double> curvedAngles;
    curvedAngles.reserve(tc);

    for (size_t t = 0; t < tc; ++t) {
        const uint32_t i0 = md.indices[t * 3], i1 = md.indices[t * 3 + 1], i2 = md.indices[t * 3 + 2];
        if (i0 >= vc || i1 >= vc || i2 >= vc) continue;
        const double ax = md.positions[i0 * 3], ay = md.positions[i0 * 3 + 1], az = md.positions[i0 * 3 + 2];
        const double bx = md.positions[i1 * 3] - ax, by = md.positions[i1 * 3 + 1] - ay, bz = md.positions[i1 * 3 + 2] - az;
        const double cx = md.positions[i2 * 3] - ax, cy = md.positions[i2 * 3 + 1] - ay, cz = md.positions[i2 * 3 + 2] - az;
        double fnx = by * cz - bz * cy, fny = bz * cx - bx * cz, fnz = bx * cy - by * cx;
        const double len = std::sqrt(fnx * fnx + fny * fny + fnz * fnz);
        if (len < 1e-8) continue;
        fnx /= len; fny /= len; fnz /= len;

        const uint32_t w[3] = {weld[i0], weld[i1], weld[i2]};
        if (w[0] == w[1] || w[1] == w[2] || w[2] == w[0]) continue;
        for (int e = 0; e < 3; ++e) {
            const uint32_t u = std::min(w[e], w[(e + 1) % 3]), v = std::max(w[e], w[(e + 1) % 3]);
            const uint64_t key = (static_cast<uint64_t>(u) << 32) | v;
            auto& entry = edgeMap[key];
            if (entry.count == 0) {
                entry.nx = fnx; entry.ny = fny; entry.nz = fnz; entry.count = 1;
            } else if (entry.count == 1) {
                entry.count = 2;
                const double dot = std::clamp(entry.nx * fnx + entry.ny * fny + entry.nz * fnz, -1.0, 1.0);
                const double deg = std::acos(dot) * (180.0 / 3.14159265358979323846);
                if (deg > 1.0 && deg < 45.0) curvedAngles.push_back(deg);
            }
        }
    }
    outCurvedEdges = static_cast<int>(curvedAngles.size());
    if (!curvedAngles.empty()) {
        std::nth_element(curvedAngles.begin(), curvedAngles.begin() + curvedAngles.size() / 2, curvedAngles.end());
        outMedianDeg = curvedAngles[curvedAngles.size() / 2];
    }
}

EyeImage extractAnnotatedCrop(const EyeImageView& beauty, const CropBox& cb) {
    const int cw = std::max(1, cb.x1 - cb.x0);
    const int ch = std::max(1, cb.y1 - cb.y0);
    const int padX = std::max(24, cw / 3);
    const int padY = std::max(24, ch / 3);
    int x0 = std::max(0, cb.x0 - padX), y0 = std::max(0, cb.y0 - padY);
    int x1 = std::min(beauty.width, cb.x1 + padX), y1 = std::min(beauty.height, cb.y1 + padY);
    if (x1 - x0 < 96) {
        const int cx = (x0 + x1) / 2;
        x0 = std::max(0, cx - 48);
        x1 = std::min(beauty.width, x0 + 96);
    }
    if (y1 - y0 < 96) {
        const int cy = (y0 + y1) / 2;
        y0 = std::max(0, cy - 48);
        y1 = std::min(beauty.height, y0 + 96);
    }
    const int w = std::max(1, x1 - x0), h = std::max(1, y1 - y0);
    EyeImage out{w, h, std::vector<uint8_t>(static_cast<size_t>(w) * h * 4, 255)};
    const int stride = beauty.effectiveStride();
    for (int y = 0; y < h; ++y) {
        std::memcpy(out.rgba.data() + static_cast<size_t>(y) * w * 4,
                    beauty.rgba + static_cast<size_t>(y0 + y) * stride + static_cast<size_t>(x0) * 4,
                    static_cast<size_t>(w) * 4);
    }
    const uint8_t cr = 255, cg = cb.isError ? 72 : 196, cbCol = cb.isError ? 72 : 48;
    draw::drawRectBox(out, cb.x0 - x0, cb.y0 - y0, std::max(2, cb.x1 - cb.x0), std::max(2, cb.y1 - cb.y0), 2, cr, cg, cbCol);
    return out;
}

}  // namespace

EyeDiagnoseResult diagnoseScene(scene::SceneGraph& graph, const EyeDiagnoseOptions& opts) {
    EyeDiagnoseResult res;
    const int savedDebug = graph.renderer().debugView();

    auto capDebug = [&](int debugId, int& outW, int& outH) -> std::vector<uint8_t> {
        graph.renderer().setDebugView(debugId);
        graph.renderForRead();
        return graph.readTonemapPixelsRGBA(outW, outH);
    };

    const int idAlbedo = scene::debug_views::find("albedo");
    const int idRough = scene::debug_views::find("roughness");
    const int idNormals = scene::debug_views::find("normals");
    const int idObjId = scene::debug_views::find("id");
    const int idTransId = scene::debug_views::find("id_translucent");

    const bool hasTranslucent = subtreeHasTranslucent(graph.root());

    int w = 0, h = 0, aw = 0, ah = 0, rw = 0, rh = 0, nw = 0, nh = 0, iw = 0, ih = 0;
    const auto beauty = capDebug(0, w, h);
    const auto albedo = capDebug(idAlbedo, aw, ah);
    const auto rough = capDebug(idRough, rw, rh);
    const auto norms = capDebug(idNormals, nw, nh);
    const auto ids = capDebug(idObjId, iw, ih);
    std::vector<uint8_t> transIds;
    int itw = 0, ith = 0;
    if (hasTranslucent && idTransId >= 0) {
        transIds = capDebug(idTransId, itw, ith);
    }
    graph.renderer().setDebugView(savedDebug);

    if (beauty.empty() || w <= 2 || h <= 2 || aw != w || ah != h || rw != w || rh != h || nw != w || nh != h || iw != w || ih != h) {
        res.ok = false;
        return res;
    }

    res.width = w;
    res.height = h;
    const size_t totalPx = static_cast<size_t>(w) * h;

    std::vector<uint32_t> idBuf(totalPx, 0);
    for (size_t i = 0; i < totalPx; ++i) {
        const size_t o = i * 4;
        idBuf[i] = static_cast<uint32_t>(ids[o]) |
                   (static_cast<uint32_t>(ids[o + 1]) << 8) |
                   (static_cast<uint32_t>(ids[o + 2]) << 16);
    }

    std::unordered_map<uint32_t, Accum> mapAcc;
    std::vector<uint32_t> order;

    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const size_t idx = static_cast<size_t>(y) * w + x;
            const uint32_t nid = idBuf[idx];
            if (nid == 0) continue;
            // Strict 4-neighbor interior filter eliminates MSAA/FXAA edge blending contamination
            if (idBuf[idx - 1] != nid || idBuf[idx + 1] != nid ||
                idBuf[idx - w] != nid || idBuf[idx + w] != nid) continue;

            auto it = mapAcc.find(nid);
            if (it == mapAcc.end()) {
                scene::SceneNode* sn = graph.findById(nid);
                if (!sn || (sn->type() != scene::SceneNode::Type::Mesh &&
                            sn->type() != scene::SceneNode::Type::InstancedMesh)) {
                    continue;
                }
                Accum ac;
                ac.id = nid;
                ac.name = sn->name().empty() ? ("node_" + std::to_string(nid)) : sn->name();
                ac.info = inspectDiagNode(sn);
                order.push_back(nid);
                it = mapAcc.emplace(nid, std::move(ac)).first;
            }
            Accum& ac = it->second;
            const size_t o = idx * 4;
            ac.minX = std::min(ac.minX, x); ac.maxX = std::max(ac.maxX, x);
            ac.minY = std::min(ac.minY, y); ac.maxY = std::max(ac.maxY, y);
            ++ac.count;

            const double bLum = srgbLum(beauty.data() + o);
            const double aLum = srgbLum(albedo.data() + o);
            ac.lumHist[std::clamp(static_cast<int>(std::lround(bLum * 255.0)), 0, 255)] += 1;
            ac.alR += albedo[o] / 255.0;
            ac.alG += albedo[o + 1] / 255.0;
            ac.alB += albedo[o + 2] / 255.0;
            ac.alLum += aLum;
            if (bLum > 0.90 && aLum < 0.78) ++ac.clippedCount;

            const double rg = std::pow(rough[o] / 255.0, 2.2);
            ac.rSum += rg;
            ac.rSq += rg * rg;

            for (int c = 0; c < 3; ++c) {
                const double nv = norms[o + c] / 255.0;
                ac.nSum[c] += nv;
                ac.nSq[c] += nv * nv;
            }

            if (idBuf[idx + 1] == nid && idBuf[idx + w] == nid) {
                const double gx = srgbLum(beauty.data() + o + 4) - srgbLum(beauty.data() + o - 4);
                const double gy = srgbLum(beauty.data() + o + static_cast<size_t>(w) * 4) -
                                  srgbLum(beauty.data() + o - static_cast<size_t>(w) * 4);
                const double gmag = std::hypot(gx, gy);
                ac.gradSum += gmag;
                ++ac.gradCnt;
                if (gmag > 0.015) {
                    const double ang = std::abs(std::atan2(gy, gx));
                    const bool cardinal = (ang < 0.26) || (ang > 2.88) || (std::abs(ang - 1.5707963) < 0.26);
                    if (cardinal) ac.axisGrad += gmag;
                    ac.totalGrad += gmag;
                }
            }
        }
    }

    const int minPx = std::max(1, opts.minPixels);
    std::unordered_map<uint32_t, std::pair<double, int>> brightOutliers;
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const size_t idx = static_cast<size_t>(y) * w + x;
            const uint32_t nid = idBuf[idx];
            if (nid == 0) continue;
            auto it = mapAcc.find(nid);
            if (it == mapAcc.end() || it->second.count < minPx) continue;
            const double p50 = histQuantile(it->second.lumHist, it->second.count, 0.50);
            const size_t o = idx * 4;
            const double bLum = srgbLum(beauty.data() + o);
            const double aLum = srgbLum(albedo.data() + o);
            if (bLum > std::max(0.88, p50 + 0.45) && aLum < 0.78) {
                brightOutliers[nid].second += 1;
            }
        }
    }

    std::vector<CropBox> cropBoxes;
    auto pushAnomaly = [&](std::string kind, std::string sev, const Accum& ac, double val, const std::string& msg) {
        EyeDiagnoseAnomaly an;
        an.kind = std::move(kind);
        an.severity = std::move(sev);
        an.nodeName = ac.name;
        an.nodeId = ac.id;
        an.value = val;
        an.bbox[0] = ac.minX; an.bbox[1] = ac.minY; an.bbox[2] = ac.maxX; an.bbox[3] = ac.maxY;
        an.message = msg;
        res.anomalies.push_back(std::move(an));
        cropBoxes.push_back({ac.name + ":" + kind, an.severity == "error", ac.minX, ac.minY, ac.maxX + 1, ac.maxY + 1});
    };

    for (uint32_t nid : order) {
        const Accum& ac = mapAcc[nid];
        if (ac.count < minPx) continue;
        ++res.visibleMeshes;

        EyeDiagnoseMeshStats ms;
        ms.name = ac.name;
        ms.nodeId = ac.id;
        ms.screenPx = ac.count;
        ms.screenFrac = static_cast<double>(ac.count) / static_cast<double>(totalPx);
        ms.p10Lum = histQuantile(ac.lumHist, ac.count, 0.10);
        ms.p50Lum = histQuantile(ac.lumHist, ac.count, 0.50);
        ms.p95Lum = histQuantile(ac.lumHist, ac.count, 0.95);
        ms.p99Lum = histQuantile(ac.lumHist, ac.count, 0.99);
        const double invN = 1.0 / static_cast<double>(ac.count);
        ms.roughnessMean = ac.rSum * invN;
        ms.roughnessStd = std::sqrt(std::max(0.0, ac.rSq * invN - ms.roughnessMean * ms.roughnessMean));
        ms.albedoLumMean = ac.alLum * invN;
        ms.detailEntropy = ac.gradCnt > 0 ? ac.gradSum / ac.gradCnt : 0.0;
        double nVarSum = 0.0;
        for (int c = 0; c < 3; ++c) {
            const double nm = ac.nSum[c] * invN;
            nVarSum += std::max(0.0, ac.nSq[c] * invN - nm * nm);
        }
        ms.normalStd = std::sqrt(nVarSum / 3.0);
        ms.axisGridScore = ac.totalGrad > 1e-4 ? ac.axisGrad / ac.totalGrad : 0.0;
        const int outCnt = brightOutliers.count(nid) ? brightOutliers[nid].second : 0;
        ms.outlierBrightFrac = outCnt * invN;

        const bool isEmissive = ac.info.valid && ac.info.emissive > 0.5f;
        const bool isUnlit = ac.info.valid && ac.info.effectiveUnlit;
        ms.instanced = ac.info.instanced;
        ms.instanceCount = ac.info.instanceCount;
        if (ac.info.valid) {
            ms.hasAlbedoMap = ac.info.hasAlbedoMap;
            ms.hasNormalMap = ac.info.hasNormalMap;
            ms.hasMRMap = ac.info.hasMRMap;
            ms.castsShadow = ac.info.castsShadow;
        }

        if (!isEmissive && !isUnlit && outCnt >= 4 && ms.outlierBrightFrac > 0.008) {
            char msg[160];
            std::snprintf(msg, sizeof(msg), "p50=%.2f p99=%.2f brightOutlier=%.1f%%", ms.p50Lum, ms.p99Lum, ms.outlierBrightFrac * 100.0);
            pushAnomaly("bright_outlier", "error", ac, ms.outlierBrightFrac, msg);
        }

        if (ac.info.valid && ms.hasAlbedoMap && !ac.info.hasCustomShader && !ac.info.atlasTiled && ac.info.baseColorTex) {
            double texR = 0, texG = 0, texB = 0;
            if (sampleTextureMeanSrgb(*ac.info.baseColorTex, ac.info.color, texR, texG, texB)) {
                double th = 0, ts = 0, tv = 0, rh = 0, rs = 0, rv = 0;
                rgbToHsv(texR, texG, texB, th, ts, tv);
                rgbToHsv(ac.alR * invN, ac.alG * invN, ac.alB * invN, rh, rs, rv);
                ms.hueShiftDeg = wrapHueDiff(th, rh);
                ms.chromaDelta = std::abs(rs - ts);
                const bool hueBad = (ts > 0.22 && rs > 0.22 && ms.hueShiftDeg > 45.0);
                const bool chromaBad = (ms.chromaDelta > 0.38 && rs > 0.45 && ac.info.vertexColorMode != 0);
                if (hueBad || chromaBad) {
                    char msg[160];
                    std::snprintf(msg, sizeof(msg), "hueShift=%.0fdeg chromaDelta=%.2f (check vertexColorTint)", ms.hueShiftDeg, ms.chromaDelta);
                    pushAnomaly("tint_mismatch", "error", ac, std::max(ms.hueShiftDeg, ms.chromaDelta * 100.0), msg);
                }
            }
        }

        if (ac.info.valid && !isEmissive && !isUnlit && ac.count >= 400) {
            const bool flatShading = ac.info.hasCustomShader
                ? (ms.detailEntropy < 0.010 && ms.normalStd < 0.030)
                : (!ms.hasAlbedoMap || !ms.hasNormalMap || (ms.detailEntropy < 0.010 && ms.normalStd < 0.030));
            if (flatShading) {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "albedo=%d normal=%d entropy=%.3f nStd=%.3f",
                              ms.hasAlbedoMap ? 1 : 0, ms.hasNormalMap ? 1 : 0, ms.detailEntropy, ms.normalStd);
                pushAnomaly("flat_plastic", ms.hasAlbedoMap ? "warning" : "error", ac, ms.detailEntropy, msg);
            }
        }

        if (ac.count >= 1200 && ms.axisGridScore > 0.72 && ms.detailEntropy > 0.03 && !isArchitecturalOrUrbanName(ac.name)) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "axisGrid=%.2f entropy=%.3f", ms.axisGridScore, ms.detailEntropy);
            pushAnomaly("axis_grid", "warning", ac, ms.axisGridScore, msg);
        }

        if (ac.info.valid && !isEmissive && !isUnlit && ms.screenFrac < 0.25 && ac.info.color[3] >= 0.99f && !ms.castsShadow && !isGroundOrTerrainName(ac.name)) {
            pushAnomaly("no_shadow_caster", "warning", ac, 1.0, "opaque prop has castsShadow=false");
        }

        res.meshes.push_back(std::move(ms));
    }

    // Translucent mesh ID attribution + coarse facet / directional striation check
    if (!transIds.empty() && itw == w && ith == h) {
        std::vector<uint32_t> transBuf(totalPx, 0);
        for (size_t i = 0; i < totalPx; ++i) {
            const size_t o = i * 4;
            transBuf[i] = static_cast<uint32_t>(transIds[o]) |
                          (static_cast<uint32_t>(transIds[o + 1]) << 8) |
                          (static_cast<uint32_t>(transIds[o + 2]) << 16);
        }
        struct TransAcc {
            Accum base;
            double d2Sum = 0.0;
            int flipCnt = 0;
            int sampleCnt = 0;
        };
        std::unordered_map<uint32_t, TransAcc> tMap;
        std::vector<uint32_t> tOrder;
        for (int y = 1; y + 1 < h; ++y) {
            for (int x = 1; x + 1 < w; ++x) {
                const size_t idx = static_cast<size_t>(y) * w + x;
                const uint32_t nid = transBuf[idx];
                if (nid == 0) continue;
                if (transBuf[idx - 1] != nid || transBuf[idx + 1] != nid ||
                    transBuf[idx - w] != nid || transBuf[idx + w] != nid) continue;
                auto it = tMap.find(nid);
                if (it == tMap.end()) {
                    scene::SceneNode* sn = graph.findById(nid);
                    if (!sn || (sn->type() != scene::SceneNode::Type::Mesh &&
                                sn->type() != scene::SceneNode::Type::InstancedMesh)) {
                        continue;
                    }
                    TransAcc ta;
                    ta.base.id = nid;
                    ta.base.name = sn->name().empty() ? ("node_" + std::to_string(nid)) : sn->name();
                    ta.base.info = inspectDiagNode(sn);
                    tOrder.push_back(nid);
                    it = tMap.emplace(nid, std::move(ta)).first;
                }
                TransAcc& ta = it->second;
                const size_t o = idx * 4;
                ta.base.minX = std::min(ta.base.minX, x); ta.base.maxX = std::max(ta.base.maxX, x);
                ta.base.minY = std::min(ta.base.minY, y); ta.base.maxY = std::max(ta.base.maxY, y);
                ++ta.base.count;

                const double bLum = srgbLum(beauty.data() + o);
                ta.base.lumHist[std::clamp(static_cast<int>(std::lround(bLum * 255.0)), 0, 255)] += 1;

                const double lx0 = srgbLum(beauty.data() + o - 4), lx1 = srgbLum(beauty.data() + o + 4);
                const double ly0 = srgbLum(beauty.data() + o - static_cast<size_t>(w) * 4);
                const double ly1 = srgbLum(beauty.data() + o + static_cast<size_t>(w) * 4);
                const double d2x = std::abs(lx0 + lx1 - 2.0 * bLum);
                const double d2y = std::abs(ly0 + ly1 - 2.0 * bLum);
                ta.d2Sum += 0.5 * (d2x + d2y);
                const double gx0 = bLum - lx0, gx1 = lx1 - bLum;
                const double gy0 = bLum - ly0, gy1 = ly1 - bLum;
                if ((gx0 * gx1 < -2e-5) || (gy0 * gy1 < -2e-5)) ++ta.flipCnt;
                ++ta.sampleCnt;
            }
        }

        for (uint32_t nid : tOrder) {
            const TransAcc& ta = tMap[nid];
            if (ta.base.count < minPx) continue;
            if (!mapAcc.count(nid)) ++res.visibleMeshes;

            EyeDiagnoseMeshStats ms;
            ms.name = ta.base.name;
            ms.nodeId = nid;
            ms.screenPx = ta.base.count;
            ms.screenFrac = static_cast<double>(ta.base.count) / static_cast<double>(totalPx);
            ms.p10Lum = histQuantile(ta.base.lumHist, ta.base.count, 0.10);
            ms.p50Lum = histQuantile(ta.base.lumHist, ta.base.count, 0.50);
            ms.p95Lum = histQuantile(ta.base.lumHist, ta.base.count, 0.95);
            ms.p99Lum = histQuantile(ta.base.lumHist, ta.base.count, 0.99);
            ms.translucent = true;
            ms.instanced = ta.base.info.instanced;
            ms.instanceCount = ta.base.info.instanceCount;
            if (ta.base.info.valid) {
                ms.hasAlbedoMap = ta.base.info.hasAlbedoMap;
                ms.hasNormalMap = ta.base.info.hasNormalMap;
                ms.hasMRMap = ta.base.info.hasMRMap;
                ms.castsShadow = ta.base.info.castsShadow;
            }
            const double span = std::max(0.08, ms.p95Lum - ms.p10Lum);
            const double ridgeMean = ta.sampleCnt > 0 ? (ta.d2Sum / ta.sampleCnt) / span : 0.0;
            const double flipFrac = ta.sampleCnt > 0 ? static_cast<double>(ta.flipCnt) / ta.sampleCnt : 0.0;
            ms.striationScore = ridgeMean * 0.7 + flipFrac * 0.3;

            int curvedEdges = 0;
            double medianDeg = 0.0;
            const float effAlpha = ta.base.info.color[3];
            const bool twoSidedLowAlpha = ta.base.info.valid && ta.base.info.twoSided && effAlpha < 0.60f;
            if (ta.base.info.valid && ta.base.info.facetMesh && effAlpha < 0.65f) {
                computeCurvedFacetStats(*ta.base.info.facetMesh, curvedEdges, medianDeg);
            }
            const bool coarsePolys = twoSidedLowAlpha && curvedEdges >= 24 && medianDeg >= 10.0;
            const bool visualStriation = (ta.base.count >= 400 && ms.striationScore > 0.22 && (twoSidedLowAlpha || medianDeg >= 10.0));
            if (coarsePolys || visualStriation) {
                char msg[192];
                std::snprintf(msg, sizeof(msg),
                              "translucent striations (twoSided=%d, medianDihedral=%.1fdeg, striation=%.2f)",
                              twoSidedLowAlpha ? 1 : 0, medianDeg, ms.striationScore);
                const std::string sev = (twoSidedLowAlpha && medianDeg >= 12.0) ? "error" : "warning";
                pushAnomaly("translucent_striation", sev, ta.base,
                            coarsePolys ? medianDeg : ms.striationScore * 100.0, msg);
            }
            res.meshes.push_back(std::move(ms));
        }
    }

    // Detect global HDR bloom/specular hotspot clusters (even if not owned by an opaque ID)
    std::vector<uint8_t> hotMask(totalPx, 0);
    std::vector<uint8_t> visited(totalPx, 0);
    for (size_t i = 0; i < totalPx; ++i) {
        const size_t o = i * 4;
        if (rough[o] < 6) continue;
        if (srgbLum(beauty.data() + o) > 0.96 && srgbLum(albedo.data() + o) < 0.78) hotMask[i] = 1;
    }
    const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
    std::vector<size_t> q;
    for (size_t i = 0; i < totalPx; ++i) {
        if (!hotMask[i] || visited[i]) continue;
        visited[i] = 1;
        q.clear();
        q.push_back(i);
        size_t head = 0;
        int minX = w, maxX = 0, minY = h, maxY = 0;
        uint32_t domId = 0;
        while (head < q.size()) {
            const size_t cur = q[head++];
            const int cx = static_cast<int>(cur % w), cy = static_cast<int>(cur / w);
            minX = std::min(minX, cx); maxX = std::max(maxX, cx);
            minY = std::min(minY, cy); maxY = std::max(maxY, cy);
            if (domId == 0 && idBuf[cur] != 0) domId = idBuf[cur];
            for (int d = 0; d < 4; ++d) {
                const int nx = cx + dx4[d], ny = cy + dy4[d];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                const size_t ni = static_cast<size_t>(ny) * w + nx;
                if (hotMask[ni] && !visited[ni]) {
                    visited[ni] = 1;
                    q.push_back(ni);
                }
            }
        }
        if (q.size() >= 3) {
            ++res.bloomHotspotCount;
            scene::SceneNode* sn = domId != 0 ? graph.findById(domId) : nullptr;
            const DiagNodeInfo dn = inspectDiagNode(sn);
            if (!dn.valid || dn.emissive <= 0.5f) {
                EyeDiagnoseAnomaly ha;
                ha.kind = "bloom_hotspot";
                ha.severity = "error";
                ha.nodeName = sn ? sn->name() : "scene";
                ha.nodeId = domId;
                ha.value = static_cast<double>(q.size());
                ha.bbox[0] = minX; ha.bbox[1] = minY; ha.bbox[2] = maxX; ha.bbox[3] = maxY;
                ha.message = "clipped HDR spike cluster (" + std::to_string(q.size()) + " px)";
                res.anomalies.push_back(std::move(ha));
                cropBoxes.push_back({"hotspot", true, minX, minY, maxX + 1, maxY + 1});
            }
        }
    }

    std::sort(res.anomalies.begin(), res.anomalies.end(), [](const EyeDiagnoseAnomaly& a, const EyeDiagnoseAnomaly& b) {
        if (a.severity != b.severity) return a.severity == "error";
        return a.value > b.value;
    });
    res.anomalyCount = static_cast<int>(res.anomalies.size());

    if (!opts.path.empty() && (!res.anomalies.empty() || opts.alwaysRender)) {
        const EyeImageView bView{beauty.data(), w, h, w * 4};
        std::vector<EyeImage> panels;
        std::vector<std::string> labels;
        const size_t maxCrops = std::min<size_t>(cropBoxes.size(), 4);
        for (size_t i = 0; i < maxCrops; ++i) {
            panels.push_back(extractAnnotatedCrop(bView, cropBoxes[i]));
            labels.push_back(cropBoxes[i].label);
        }
        if (panels.empty()) {
            panels.push_back(draw::fitBox(bView, 768, 432));
            labels.push_back("clean | 0 anomalies");
        }
        const int cols = panels.size() == 1 ? 1 : 2;
        const int rows = static_cast<int>((panels.size() + cols - 1) / cols);
        const int sheetW = std::clamp(opts.sheetWidth > 0 ? opts.sheetWidth : 768, 256, 1280);
        const int defH = panels.size() <= 2 ? 384 : 768;
        const int sheetH = std::clamp(opts.sheetHeight > 0 ? opts.sheetHeight : defH, 256, 1280);
        const auto sheet = draw::assembleExactSheet(panels, labels, cols, rows, sheetW, sheetH, 1);
        res.visual = {sheet.width, sheet.height, std::move(sheet.rgba)};
        std::error_code ec;
        const std::filesystem::path outPath(opts.path);
        if (outPath.has_parent_path()) std::filesystem::create_directories(outPath.parent_path(), ec);
        if (broimage::encode_png_file(opts.path, res.visual.rgba.data(), res.visual.width, res.visual.height, 4)) {
            res.visualPath = opts.path;
        }
    }

    return res;
}

}  // namespace bro::eye
