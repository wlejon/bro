// =============================================================================
// bro_tile_world_c_abi.cpp — C++ forwarding implementations for bro.tile_world
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_tile_world_c_abi.h"
#include <cstdint>
#include <unordered_map>

struct BroTileWorldImpl {
    int32_t width = 256;
    int32_t height = 256;
    int32_t chunkCount = 16;
    bool paging = false;
    int32_t vertexCount = 0;
    int32_t triangleCount = 0;
    double originX = 0.0, originY = 0.0, originZ = 0.0;
    double camX = 0.0, camY = 0.0, camZ = 0.0;
    std::unordered_map<uint64_t, int32_t> tiles;

    static uint64_t makeKey(int32_t layer, int32_t x, int32_t y) {
        return (static_cast<uint64_t>(layer) << 32) |
               ((static_cast<uint64_t>(x) & 0xFFFF) << 16) |
               (static_cast<uint64_t>(y) & 0xFFFF);
    }
};

extern "C" {

void* bro_TileWorld_create(void) {
    return new BroTileWorldImpl();
}

void bro_TileWorld_destroy(void* self) {
    delete static_cast<BroTileWorldImpl*>(self);
}

void* bro_TileWorld_get_node(void* /*self*/) {
    return nullptr;
}

int32_t bro_TileWorld_get_width(void* self) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    return w ? w->width : 0;
}

int32_t bro_TileWorld_get_height(void* self) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    return w ? w->height : 0;
}

int32_t bro_TileWorld_get_chunkCount(void* self) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    return w ? w->chunkCount : 0;
}

int32_t bro_TileWorld_get_chunks(void* self) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    return w ? w->chunkCount : 0;
}

bool bro_TileWorld_get_paging(void* self) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    return w ? w->paging : false;
}

void bro_TileWorld_set_paging(void* self, bool val) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    if (w) w->paging = val;
}

int32_t bro_TileWorld_get_vertexCount(void* self) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    return w ? w->vertexCount : 0;
}

int32_t bro_TileWorld_get_triangleCount(void* self) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    return w ? w->triangleCount : 0;
}

int32_t bro_TileWorld_update(void* self, double camX, double camY, double camZ) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    if (w) {
        w->camX = camX;
        w->camY = camY;
        w->camZ = camZ;
        return w->chunkCount;
    }
    return 0;
}

void bro_TileWorld_setTile(void* self, int32_t layer, int32_t x, int32_t y, int32_t tileId) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    if (w) {
        w->tiles[BroTileWorldImpl::makeKey(layer, x, y)] = tileId;
    }
}

int32_t bro_TileWorld_getTile(void* self, int32_t layer, int32_t x, int32_t y) {
    auto* w = static_cast<BroTileWorldImpl*>(self);
    if (w) {
        auto it = w->tiles.find(BroTileWorldImpl::makeKey(layer, x, y));
        if (it != w->tiles.end()) return it->second;
    }
    return 0;
}

void bro_TileWorld_fillRect(void* self, int32_t layer, int32_t x, int32_t y, int32_t w, int32_t h, int32_t tileId) {
    auto* tw = static_cast<BroTileWorldImpl*>(self);
    if (!tw) return;
    for (int32_t iy = 0; iy < h; ++iy) {
        for (int32_t ix = 0; ix < w; ++ix) {
            tw->tiles[BroTileWorldImpl::makeKey(layer, x + ix, y + iy)] = tileId;
        }
    }
}

void bro_TileWorld_clearLayer(void* self, int32_t layer) {
    auto* tw = static_cast<BroTileWorldImpl*>(self);
    if (!tw) return;
    uint64_t layerMask = static_cast<uint64_t>(layer) << 32;
    for (auto it = tw->tiles.begin(); it != tw->tiles.end(); ) {
        if ((it->first & 0xFFFFFFFF00000000ULL) == layerMask) {
            it = tw->tiles.erase(it);
        } else {
            ++it;
        }
    }
}

void* bro_TileWorld_pickTile(void* /*self*/, double /*worldX*/, double /*worldZ*/) { return nullptr; }
void* bro_TileWorld_findPath(void* /*self*/, int32_t /*startX*/, int32_t /*startY*/, int32_t /*endX*/, int32_t /*endY*/, void* /*opts*/) { return nullptr; }
void* bro_TileWorld_computeRegions(void* /*self*/, int32_t /*layer*/) { return nullptr; }
void bro_TileWorld_applyAutotile(void* /*self*/, int32_t /*layer*/, void* /*rules*/) {}
void* bro_TileWorld_extractVoxelMesh(void* /*self*/, void* /*opts*/) { return nullptr; }

void bro_TileWorld_setOrigin(void* self, double x, double y, double z) {
    auto* tw = static_cast<BroTileWorldImpl*>(self);
    if (tw) { tw->originX = x; tw->originY = y; tw->originZ = z; }
}

bool bro_TileWorld_advance(void* /*self*/, double /*dtMs*/) {
    return true;
}

void bro_TileWorld_addObjectKind(void* /*self*/, int32_t /*kindId*/, void* /*spec*/) {}
void bro_TileWorld_addObject(void* /*self*/, int32_t /*kindId*/, double /*x*/, double /*y*/, double /*z*/) {}
void bro_TileWorld_clearObjects(void* /*self*/, int32_t /*kindId*/) {}
int32_t bro_TileWorld_objectCount(void* /*self*/, int32_t /*kind*/) { return 0; }
void bro_TileWorld_rebuildObjects(void* /*self*/) {}
void bro_TileWorld_rebuild(void* /*self*/) {}
void bro_TileWorld_rebuildAll(void* /*self*/) {}
void bro_TileWorld_configure(void* /*self*/, void* /*cfg*/) {}
void* bro_TileWorld_save(void* /*self*/) { return nullptr; }
bool bro_TileWorld_load(void* /*self*/, void* /*data*/) { return true; }

} // extern "C"
