#pragma once

#include <memory>

#if BRO_WITH_GAMEAI

namespace brogameagent { class NavMesh; }

namespace bro::engine {

void registerNavMeshForPump(const std::shared_ptr<brogameagent::NavMesh>& mesh);
void pumpNavMeshObstacles(float dt);

} // namespace bro::engine

#else

namespace brogameagent { class NavMesh; }

namespace bro::engine {

inline void registerNavMeshForPump(const std::shared_ptr<brogameagent::NavMesh>&) {}
inline void pumpNavMeshObstacles(float) {}

} // namespace bro::engine

#endif
