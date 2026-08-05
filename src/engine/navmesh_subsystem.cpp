#include "engine/navmesh_subsystem.h"
#include <brogameagent/nav_mesh.h>
#include <mutex>
#include <vector>

namespace bro::engine {

static std::mutex g_navMeshPumpMutex;
static std::vector<std::weak_ptr<brogameagent::NavMesh>> g_navMeshPump;

void registerNavMeshForPump(const std::shared_ptr<brogameagent::NavMesh>& m) {
    std::lock_guard<std::mutex> lock(g_navMeshPumpMutex);
    g_navMeshPump.push_back(m);
}

void pumpNavMeshObstacles(float dt) {
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
#if BROGAMEAGENT_HAS_NAVMESH
    for (auto& m : live) {
        while (m->obstaclesPending()) {
            m->update(dt);
        }
    }
#else
    (void)dt;
#endif
}

} // namespace bro::engine
