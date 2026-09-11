// =============================================================================
// bro_ai_c_abi.h — Pure C-ABI declarations for bro.ai
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// Zero dynamic boxing, direct native CPU register call.
// =============================================================================

#ifndef BRO_AI_C_ABI_H
#define BRO_AI_C_ABI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface bro.ai.AIAgent ---
void* bro_AIAgent_create(void);
void  bro_AIAgent_destroy(void* self);
int32_t bro_AIAgent_get_id(void* self);
void bro_AIAgent_setPosition(void* self, double x, double y, double z);
void bro_AIAgent_setVelocity(void* self, double x, double y, double z);
void bro_AIAgent_setGoal(void* self, double x, double y, double z);
void bro_AIAgent_stop(void* self);

// --- Interface bro.ai.AIWorld ---
void* bro_AIWorld_create(void);
void  bro_AIWorld_destroy(void* self);
void bro_AIWorld_step(void* self, double dt);
void* bro_AIWorld_createAgent(void* self, void* opts);
void bro_AIWorld_destroyAgent(void* self, void* agent);

// --- Interface bro.ai.AINavGrid ---
void* bro_AINavGrid_create(void);
void  bro_AINavGrid_destroy(void* self);
bool bro_AINavGrid_isWalkable(void* self, double x, double z);
void bro_AINavGrid_setWalkable(void* self, double x, double z, bool walkable);
void* bro_AINavGrid_findPath(void* self, double startX, double startZ, double endX, double endZ);

// --- Interface bro.ai.AINavMesh ---
void* bro_AINavMesh_create(void);
void  bro_AINavMesh_destroy(void* self);
void* bro_AINavMesh_findPath(void* self, double startX, double startY, double startZ, double endX, double endY, double endZ);

// --- Interface bro.ai.AIMcts ---
void* bro_AIMcts_create(void);
void  bro_AIMcts_destroy(void* self);
void* bro_AIMcts_search(void* self, void* state, void* opts);

// --- Interface bro.ai.CombatAction ---
void* bro_CombatAction_create(void);
void  bro_CombatAction_destroy(void* self);
double bro_CombatAction_get_targetX(void* self);
void bro_CombatAction_set_targetX(void* self, double val);
double bro_CombatAction_get_targetZ(void* self);
void bro_CombatAction_set_targetZ(void* self, double val);
int32_t bro_CombatAction_get_actionType(void* self);
void bro_CombatAction_set_actionType(void* self, int32_t val);

// --- Interface bro.ai.Formation ---
void* bro_Formation_create(void);
void  bro_Formation_destroy(void* self);
void bro_Formation_setLeader(void* self, void* agent);
void bro_Formation_addFollower(void* self, void* agent, double offsetX, double offsetZ);
void bro_Formation_update(void* self, double dt);

// --- Interface bro.ai.VecSim ---
void* bro_VecSim_create(void);
void  bro_VecSim_destroy(void* self);
void bro_VecSim_step(void* self, double dt);

// --- Namespace bro.game ---
void* bro_game_createAgent(void* world, void* opts);
void* bro_game_createWorld(void* opts);
void* bro_game_createNavGrid(double minX, double minZ, double maxX, double maxZ, double cellSize);
void* bro_game_createNavMesh(void* opts);
void* bro_game_createMcts(void* config);
void* bro_game_createCombatAction(void);
void* bro_game_createFormation(void* opts);
void* bro_game_createVecSim(void* opts);
void bro_game_registerCapability(const char* name, void* definition);

#ifdef __cplusplus
}
#endif

#endif // BRO_AI_C_ABI_H
