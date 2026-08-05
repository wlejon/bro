#include "engine/navmesh_subsystem.h"

#include <mutex>
#include <vector>

#if __has_include(<brogameagent/nav_mesh.h>)
#include <brogameagent/nav_mesh.h>
#define BROGAMEAGENT_HAS_NAVMESH 1
#endif

namespace bro::engine {

static std::mutex g_navMeshPumpMutex;
static std::vector<std::weak_ptr<brogameagent::NavMesh>> g_navMeshPump;

void registerNavMeshForPump(const std::shared_ptr<brogameagent::NavMesh>& m) {
    std::lock_guard<std::mutex> lock(g_navMeshPumpMutex);
    g_navMeshPump.push_back(m);
}

void pumpNavMeshObstacles(float dt) {
#ifdef BROGAMEAGENT_HAS_NAVMESH
    // Snapshot the live meshes under the lock, run the (potentially tile-
    // rebuilding) updates outside it. Expired registry entries are pruned by
    // swap-with-back.
    std::vector<std::shared_ptr<brogameagent::NavMesh>> live;
    {
        std::lock_guard<std::mutex> lock(g_navMeshPumpMutex);
        for (size_t i = 0; i < g_navMeshPump.size();) {
            if (auto sp = g_navMeshPump[i].lock()) {
                live.push_back(std::move(sp));
                i++;
            } else {
                g_navMeshPump[i] = std::move(g_navMeshPump.back());
                g_navMeshPump.pop_back();
            }
        }
    }
    for (auto& m : live) {
        if (m->obstaclesPending()) m->update(dt);
    }
#else
    (void)dt;
#endif
}

} // namespace bro::engine
