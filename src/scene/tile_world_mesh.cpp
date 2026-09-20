#include "scene/tile_world.h"
#include "scene/tile_world_internal.h"
#include "scene/scene_graph.h"
#include "scene/mesh_node.h"

#include <bromath/vec.h>
#include <bromesh/mesh_data.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace bro::scene {

using bromath::Vec3;

static void addQuad(bromesh::MeshData& m,
                    const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& d,
                    const Vec3& n,
                    const float ca[4], const float cb[4],
                    const float cc[4], const float cd[4],
                    const float* uvs) {
    Vec3 e0{b.x - a.x, b.y - a.y, b.z - a.z};
    Vec3 e1{c.x - a.x, c.y - a.y, c.z - a.z};
    Vec3 gn{e0.y * e1.z - e0.z * e1.y,
            e0.z * e1.x - e0.x * e1.z,
            e0.x * e1.y - e0.y * e1.x};
    bool flip = (gn.x * n.x + gn.y * n.y + gn.z * n.z) < 0.0f;

    uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);
    const Vec3* vs[4] = {&a, &b, &c, &d};
    const float* cs[4] = {ca, cb, cc, cd};
    for (int i = 0; i < 4; ++i) {
        m.positions.push_back(vs[i]->x);
        m.positions.push_back(vs[i]->y);
        m.positions.push_back(vs[i]->z);
        m.normals.push_back(n.x);
        m.normals.push_back(n.y);
        m.normals.push_back(n.z);
        m.colors.push_back(cs[i][0]);
        m.colors.push_back(cs[i][1]);
        m.colors.push_back(cs[i][2]);
        m.colors.push_back(cs[i][3]);
        if (uvs) {
            m.uvs.push_back(uvs[i * 2 + 0]);
            m.uvs.push_back(uvs[i * 2 + 1]);
        }
    }
    if (!flip) {
        m.indices.insert(m.indices.end(),
            {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
    } else {
        m.indices.insert(m.indices.end(),
            {base + 0, base + 2, base + 1, base + 0, base + 3, base + 2});
    }
}

static void addFan(bromesh::MeshData& m, const Vec3* pts, const float* const* cols,
                   const float* uvs, int count,
                   const Vec3& n) {
    Vec3 e0{pts[1].x - pts[0].x, pts[1].y - pts[0].y, pts[1].z - pts[0].z};
    Vec3 e1{pts[2].x - pts[0].x, pts[2].y - pts[0].y, pts[2].z - pts[0].z};
    Vec3 gn{e0.y * e1.z - e0.z * e1.y,
            e0.z * e1.x - e0.x * e1.z,
            e0.x * e1.y - e0.y * e1.x};
    bool flip = (gn.x * n.x + gn.y * n.y + gn.z * n.z) < 0.0f;

    uint32_t base = static_cast<uint32_t>(m.positions.size() / 3);
    for (int i = 0; i < count; ++i) {
        m.positions.push_back(pts[i].x);
        m.positions.push_back(pts[i].y);
        m.positions.push_back(pts[i].z);
        m.normals.push_back(n.x);
        m.normals.push_back(n.y);
        m.normals.push_back(n.z);
        m.colors.push_back(cols[i][0]);
        m.colors.push_back(cols[i][1]);
        m.colors.push_back(cols[i][2]);
        m.colors.push_back(cols[i][3]);
        if (uvs) {
            m.uvs.push_back(uvs[i * 2 + 0]);
            m.uvs.push_back(uvs[i * 2 + 1]);
        }
    }
    for (int i = 1; i + 1 < count; ++i) {
        if (!flip)
            m.indices.insert(m.indices.end(), {base, base + i, base + static_cast<uint32_t>(i + 1)});
        else
            m.indices.insert(m.indices.end(), {base, base + static_cast<uint32_t>(i + 1), base + i});
    }
}

void TileWorld::buildChunkMesh(int ccx, int ccy) {
    auto* g = graph();
    if (!g) return;   // graph gone: nothing to mesh into
    const int idx = chunkIdx(ccx, ccy);
    Chunk& chunk = chunks_[idx];
    chunk.dirty = false;

    buildGroundMesh(ccx, ccy, chunk);

    // Overlay layers (>= 1) render as decal meshes above the ground. Resize the
    // per-chunk overlay slot list to match the layer count.
    const int overlayLayers = grid_ ? std::max(0, grid_->layerCount() - 1) : 0;
    if (static_cast<int>(chunk.overlays.size()) != overlayLayers) {
        for (auto* ov : chunk.overlays) if (ov) g->destroyNode(ov);
        chunk.overlays.assign(overlayLayers, nullptr);
    }
    for (int L = 1; L <= overlayLayers; ++L)
        buildOverlayMesh(ccx, ccy, chunk, L);

    // Note whether this chunk holds any animated tile, so advance() only
    // remeshes the chunks that actually animate.
    chunkAnimated_[idx] = 0;
    if (grid_ && !config_.animations.empty()) {
        const int cz = config_.chunkSize;
        const int x0 = ccx * cz, x1 = std::min(x0 + cz, config_.width);
        const int y0 = ccy * cz, y1 = std::min(y0 + cz, config_.height);
        const int layers = grid_->layerCount();
        for (int y = y0; y < y1 && !chunkAnimated_[idx]; ++y)
            for (int x = x0; x < x1 && !chunkAnimated_[idx]; ++x)
                for (int L = 0; L < layers; ++L)
                    if (cellIsAnimated(grid_->tile(L, {x, y}))) { chunkAnimated_[idx] = 1; break; }
    }
}

bool TileWorld::advance(double dtMs) {
    if (config_.animations.empty()) return false;
    animClock_ += dtMs;
    bool changed = false;
    for (size_t i = 0; i < config_.animations.size(); ++i) {
        const auto& a = config_.animations[i];
        int n = static_cast<int>(a.frames.size());
        if (n <= 0 || a.fps <= 0.0f) continue;
        int nf = static_cast<int>(animClock_ * (a.fps / 1000.0)) % n;
        if (nf < 0) nf += n;
        if (nf != animFrame_[i]) { animFrame_[i] = nf; changed = true; }
    }
    if (!changed) return false;
    for (int idx = 0; idx < static_cast<int>(chunks_.size()); ++idx) {
        if (idx < static_cast<int>(chunkAnimated_.size()) && chunkAnimated_[idx])
            buildChunkMesh(idx % chunksX_, idx / chunksX_);
    }
    return true;
}

void TileWorld::buildGroundMesh(int ccx, int ccy, Chunk& chunk) {
    auto* g = graph();
    if (!g) return;
    const float cs = config_.cellSize;
    const int   cz = config_.chunkSize;
    const int   x0 = ccx * cz, x1 = std::min(x0 + cz, config_.width);
    const int   y0 = ccy * cz, y1 = std::min(y0 + cz, config_.height);
    const bool  hex = grid_ && grid_->topology() == tile::Topology::Hex;

    // Chunk-local origin so baked coords stay small; the node carries the offset.
    // Hex has no clean "corner" reference the way square does, so use the first
    // cell's own pixel center — any fixed reference works since cellCenterLocal is
    // affine and everything below subtracts the same (ox, oz).
    float ox = static_cast<float>(x0) * cs;
    float oz = static_cast<float>(y0) * cs;
    if (hex) cellCenterLocal(x0, y0, ox, oz);

    const float skirtY = static_cast<float>(config_.baseLevel) * config_.heightStep;
    const bool atlas = hasAtlas();

    auto paletteColor = [&](uint16_t id, float out[3]) {
        if (atlas) { out[0] = out[1] = out[2] = 1.0f; return; }
        int n = static_cast<int>(config_.palette.size() / 4);
        if (id < n) {
            out[0] = config_.palette[id * 4 + 0];
            out[1] = config_.palette[id * 4 + 1];
            out[2] = config_.palette[id * 4 + 2];
        } else {
            out[0] = out[1] = out[2] = 0.6f;
        }
    };

    bromesh::MeshData mesh;

    if (hex) {
        const auto& corners = hexCorners();
        const auto& dirs = hexDirs();
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (!solid(x, y)) continue;

                const uint16_t groundId = grid_->tile(0, {x, y});
                const int elev = grid_->elevation({x, y});
                const float hy = static_cast<float>(elev) * config_.heightStep;

                float base[3];
                paletteColor(groundId, base);

                float tu0 = 0, tu1 = 1, tv0 = 0, tv1 = 1;
                float cu0 = 0, cu1 = 1, cv0 = 0, cv1 = 1;
                if (atlas) {
                    atlasCellRect(atlasLayerCell(x, y, 0, groundId), tu0, tu1, tv0, tv1);
                    int cliff = (config_.cliffCell >= 0) ? config_.cliffCell
                                                         : atlasCellFor(groundId);
                    atlasCellRect(cliff, cu0, cu1, cv0, cv1);
                }

                float tint[4]; cellTint(x, y, tint);
                base[0] *= tint[0]; base[1] *= tint[1]; base[2] *= tint[2];

                float cx = 0, cz = 0;
                cellCenterLocal(x, y, cx, cz);
                cx -= ox; cz -= oz;

                Vec3 corner[6];
                for (int i = 0; i < 6; ++i)
                    corner[i] = Vec3{cx + cs * corners[i].x, hy, cz + cs * corners[i].z};

                tile::Neighbors nb = tile::neighbors(tile::Topology::Hex, {x, y});
                auto higher = [&](int dir) {
                    tile::Cell c = nb[dir];
                    return solid(c.x, c.y) && grid_->elevation(c) > elev;
                };
                float col[6][4];
                float uv[12];
                for (int i = 0; i < 6; ++i) {
                    int occ = (higher((i + 5) % 6) ? 1 : 0) + (higher(i) ? 1 : 0);
                    float s = 1.0f - config_.aoStrength * (static_cast<float>(occ) / 2.0f);
                    col[i][0] = base[0] * s; col[i][1] = base[1] * s;
                    col[i][2] = base[2] * s; col[i][3] = 1.0f;
                    if (atlas) {
                        uv[i * 2 + 0] = tu0 + (tu1 - tu0) * (corners[i].x / 0.8660254f + 1.0f) * 0.5f;
                        uv[i * 2 + 1] = tv0 + (tv1 - tv0) * (corners[i].z + 1.0f) * 0.5f;
                    }
                }
                const float* colp[6] = {col[0], col[1], col[2], col[3], col[4], col[5]};
                addFan(mesh, corner, colp, atlas ? uv : nullptr, 6, {0, 1, 0});

                const float sideShade = 0.72f;
                float side[4] = {base[0] * sideShade, base[1] * sideShade, base[2] * sideShade, 1.0f};
                const float cliffUV[8] = {cu0, cv1, cu0, cv0, cu1, cv0, cu1, cv1};
                const float* cliffUVp = atlas ? cliffUV : nullptr;

                for (int i = 0; i < 6; ++i) {
                    tile::Cell nc = nb[i];
                    float ny = solid(nc.x, nc.y) ? topY(nc.x, nc.y) : skirtY;
                    if (ny >= hy) continue;
                    const Vec3& a = corner[i];
                    const Vec3& b = corner[(i + 1) % 6];
                    Vec3 nrm{dirs[i].x, 0.0f, dirs[i].z};
                    addQuad(mesh, {a.x, ny, a.z}, {a.x, hy, a.z}, {b.x, hy, b.z}, {b.x, ny, b.z},
                            nrm, side, side, side, side, cliffUVp);
                }
            }
        }
    } else {
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                if (!solid(x, y)) continue;

                const uint16_t groundId = grid_->tile(0, {x, y});
                const int elev = grid_->elevation({x, y});
                const float hy = static_cast<float>(elev) * config_.heightStep;

                float base[3];
                paletteColor(groundId, base);

                float tu0 = 0, tu1 = 1, tv0 = 0, tv1 = 1;
                float cu0 = 0, cu1 = 1, cv0 = 0, cv1 = 1;
                if (atlas) {
                    atlasCellRect(atlasLayerCell(x, y, 0, groundId), tu0, tu1, tv0, tv1);
                    int cliff = (config_.cliffCell >= 0) ? config_.cliffCell
                                                         : atlasCellFor(groundId);
                    atlasCellRect(cliff, cu0, cu1, cv0, cv1);
                }

                float tint[4]; cellTint(x, y, tint);
                base[0] *= tint[0]; base[1] *= tint[1]; base[2] *= tint[2];

                const float topUV[8] = {tu0, tv0, tu0, tv1, tu1, tv1, tu1, tv0};
                const float* topUVp = atlas ? topUV : nullptr;

                const float lx0 = static_cast<float>(x) * cs - ox;
                const float lx1 = lx0 + cs;
                const float lz0 = static_cast<float>(y) * cs - oz;
                const float lz1 = lz0 + cs;

                auto cornerAO = [&](int ex, int ez) -> float {
                    int occ = 0;
                    auto higher = [&](int nx, int ny) {
                        return solid(nx, ny) && grid_->elevation({nx, ny}) > elev;
                    };
                    if (higher(x + ex, y))      ++occ;
                    if (higher(x,      y + ez)) ++occ;
                    if (higher(x + ex, y + ez)) ++occ;
                    return 1.0f - config_.aoStrength * (static_cast<float>(occ) / 3.0f);
                };
                float s00 = cornerAO(-1, -1);
                float s10 = cornerAO(+1, -1);
                float s11 = cornerAO(+1, +1);
                float s01 = cornerAO(-1, +1);

                float c00[4] = {base[0]*s00, base[1]*s00, base[2]*s00, 1.0f};
                float c10[4] = {base[0]*s10, base[1]*s10, base[2]*s10, 1.0f};
                float c11[4] = {base[0]*s11, base[1]*s11, base[2]*s11, 1.0f};
                float c01[4] = {base[0]*s01, base[1]*s01, base[2]*s01, 1.0f};

                addQuad(mesh,
                        {lx0, hy, lz0}, {lx0, hy, lz1}, {lx1, hy, lz1}, {lx1, hy, lz0},
                        {0, 1, 0}, c00, c01, c11, c10, topUVp);

                const float sideShade = 0.72f;
                float side[4] = {base[0]*sideShade, base[1]*sideShade, base[2]*sideShade, 1.0f};

                const float cliffUV[8] = {cu0, cv1, cu0, cv0, cu1, cv0, cu1, cv1};
                const float* cliffUVp = atlas ? cliffUV : nullptr;

                auto neighbourTopY = [&](int nx, int ny) -> float {
                    return solid(nx, ny) ? topY(nx, ny) : skirtY;
                };

                if (float ny = neighbourTopY(x + 1, y); ny < hy)
                    addQuad(mesh, {lx1, ny, lz0}, {lx1, hy, lz0}, {lx1, hy, lz1}, {lx1, ny, lz1},
                            {1, 0, 0}, side, side, side, side, cliffUVp);
                if (float ny = neighbourTopY(x - 1, y); ny < hy)
                    addQuad(mesh, {lx0, ny, lz0}, {lx0, hy, lz0}, {lx0, hy, lz1}, {lx0, ny, lz1},
                            {-1, 0, 0}, side, side, side, side, cliffUVp);
                if (float ny = neighbourTopY(x, y + 1); ny < hy)
                    addQuad(mesh, {lx0, ny, lz1}, {lx0, hy, lz1}, {lx1, hy, lz1}, {lx1, ny, lz1},
                            {0, 0, 1}, side, side, side, side, cliffUVp);
                if (float ny = neighbourTopY(x, y - 1); ny < hy)
                    addQuad(mesh, {lx0, ny, lz0}, {lx0, hy, lz0}, {lx1, hy, lz0}, {lx1, ny, lz0},
                            {0, 0, -1}, side, side, side, side, cliffUVp);
            }
        }
    }

    if (mesh.empty()) {
        if (chunk.ground) { g->destroyNode(chunk.ground); chunk.ground = nullptr; }
        return;
    }

    if (!chunk.ground) {
        chunk.ground = g->createMesh("tile-chunk");
        root_->addChild(chunk.ground);
        attachShadeMap(chunk.ground);
        chunk.ground->setColor(1, 1, 1, 1);
        chunk.ground->setRoughness(0.92f);
        chunk.ground->setMetallic(0.0f);
        if (atlas)
            chunk.ground->setBaseColorTexture(config_.atlasWidth, config_.atlasHeight,
                                              config_.atlasPixels.data());
    }
    chunk.ground->setMesh(std::move(mesh));
    chunk.ground->setPosition(ox, 0.0f, oz);
}

void TileWorld::buildOverlayMesh(int ccx, int ccy, Chunk& chunk, int layer) {
    auto* g = graph();
    if (!g) return;
    MeshNode*& node = chunk.overlays[layer - 1];

    const bool atlas = hasAtlas();
    if (!atlas) {
        if (node) { g->destroyNode(node); node = nullptr; }
        return;
    }

    const float cs = config_.cellSize;
    const int   cz = config_.chunkSize;
    const int   x0 = ccx * cz, x1 = std::min(x0 + cz, config_.width);
    const int   y0 = ccy * cz, y1 = std::min(y0 + cz, config_.height);
    const bool  hex = grid_->topology() == tile::Topology::Hex;
    float ox = static_cast<float>(x0) * cs;
    float oz = static_cast<float>(y0) * cs;
    if (hex) cellCenterLocal(x0, y0, ox, oz);

    const float lift = 0.012f * cs * static_cast<float>(layer);

    bromesh::MeshData mesh;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            if (!solid(x, y)) continue;
            const uint16_t id = grid_->tile(layer, {x, y});
            if (id == 0) continue;

            const float hy = topY(x, y) + lift;

            float u0, u1, v0, v1;
            atlasCellRect(atlasLayerCell(x, y, layer, id), u0, u1, v0, v1);

            float t[4]; cellTint(x, y, t);
            float col[4] = {t[0], t[1], t[2], t[3]};

            if (hex) {
                const auto& corners = hexCorners();
                float cx = 0, cz2 = 0;
                cellCenterLocal(x, y, cx, cz2);
                cx -= ox; cz2 -= oz;
                Vec3 corner[6];
                float uv[12];
                const float* colp[6];
                for (int i = 0; i < 6; ++i) {
                    corner[i] = Vec3{cx + cs * corners[i].x, hy, cz2 + cs * corners[i].z};
                    uv[i * 2 + 0] = u0 + (u1 - u0) * (corners[i].x / 0.8660254f + 1.0f) * 0.5f;
                    uv[i * 2 + 1] = v0 + (v1 - v0) * (corners[i].z + 1.0f) * 0.5f;
                    colp[i] = col;
                }
                addFan(mesh, corner, colp, uv, 6, {0, 1, 0});
                continue;
            }

            const float lx0 = static_cast<float>(x) * cs - ox;
            const float lx1 = lx0 + cs;
            const float lz0 = static_cast<float>(y) * cs - oz;
            const float lz1 = lz0 + cs;
            const float uv[8] = {u0, v0, u0, v1, u1, v1, u1, v0};
            addQuad(mesh,
                    {lx0, hy, lz0}, {lx0, hy, lz1}, {lx1, hy, lz1}, {lx1, hy, lz0},
                    {0, 1, 0}, col, col, col, col, uv);
        }
    }

    if (mesh.empty()) {
        if (node) { g->destroyNode(node); node = nullptr; }
        return;
    }

    TileWorldConfig::OverlayStyle style;
    if (layer < static_cast<int>(config_.overlays.size()))
        style = config_.overlays[layer];

    if (!node) {
        node = g->createMesh("tile-overlay");
        root_->addChild(node);
        attachShadeMap(node);
        node->setRoughness(0.95f);
        node->setMetallic(0.0f);
        node->setDepthBias(-1.0f, -static_cast<float>(layer) - 1.0f);
        node->setCastsShadow(false);
        node->setBaseColorTexture(config_.atlasWidth, config_.atlasHeight,
                                  config_.atlasPixels.data());
    }
    node->setColor(1, 1, 1, style.opacity);
    node->setAlphaCutoff(style.alphaCutoff);
    node->setMesh(std::move(mesh));
    node->setPosition(ox, 0.0f, oz);
}

void TileWorld::rebuildDirty() {
    for (int ccy = 0; ccy < chunksY_; ++ccy)
        for (int ccx = 0; ccx < chunksX_; ++ccx)
            if (chunks_[chunkIdx(ccx, ccy)].dirty)
                buildChunkMesh(ccx, ccy);
    rebuildObjects();
}

void TileWorld::rebuildAll() {
    for (int ccy = 0; ccy < chunksY_; ++ccy)
        for (int ccx = 0; ccx < chunksX_; ++ccx)
            buildChunkMesh(ccx, ccy);
    rebuildObjects();
}

} // namespace bro::scene
