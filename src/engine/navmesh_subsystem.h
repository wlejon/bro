#pragma once

#include <memory>

namespace brogameagent { class NavMesh; }

namespace bro::engine {

void registerNavMeshForPump(const std::shared_ptr<brogameagent::NavMesh>& mesh);
void pumpNavMeshObstacles(float dt);

} // namespace bro::engine
