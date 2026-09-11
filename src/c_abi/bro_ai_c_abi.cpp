// =============================================================================
// bro_ai_c_abi.cpp — C++ forwarding implementations for bro.ai
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_ai_c_abi.h"
#include "bro/c_abi/bro_engine_c_abi.h"
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>

struct BroAIAgentImpl {
    int32_t id = 0;
    double posX = 0.0, posY = 0.0, posZ = 0.0;
    double velX = 0.0, velY = 0.0, velZ = 0.0;
    double goalX = 0.0, goalY = 0.0, goalZ = 0.0;
    bool stopped = false;
};

struct BroAIWorldImpl {
    int32_t nextAgentId = 1;
    double lastDt = 0.0;
    std::vector<BroAIAgentImpl*> agents;

    ~BroAIWorldImpl() {
        for (auto* a : agents) {
            delete a;
        }
        agents.clear();
    }
};

struct BroAINavGridImpl {
    double minX = -20.0, minZ = -20.0;
    double maxX = 20.0, maxZ = 20.0;
    double cellSize = 0.5;
    std::unordered_map<int64_t, bool> unwalkable;
};

struct BroAINavMeshImpl {
    int32_t polyCount = 0;
};

struct BroAIMctsImpl {
    int32_t iterations = 1000;
};

struct BroCombatActionImpl {
    double targetX = 0.0;
    double targetZ = 0.0;
    int32_t actionType = 0;
};

struct BroFormationImpl {
    void* leader = nullptr;
    std::vector<void*> followers;
};

struct BroVecSimImpl {
    double lastDt = 0.0;
};

extern "C" {

// --- Interface bro.ai.AIAgent ---
void* bro_AIAgent_create(void) {
    return new BroAIAgentImpl();
}

void bro_AIAgent_destroy(void* self) {
    delete static_cast<BroAIAgentImpl*>(self);
}

int32_t bro_AIAgent_get_id(void* self) {
    auto* a = static_cast<BroAIAgentImpl*>(self);
    return a ? a->id : 0;
}

void bro_AIAgent_setPosition(void* self, double x, double y, double z) {
    auto* a = static_cast<BroAIAgentImpl*>(self);
    if (a) {
        a->posX = x;
        a->posY = y;
        a->posZ = z;
    }
}

void bro_AIAgent_setVelocity(void* self, double x, double y, double z) {
    auto* a = static_cast<BroAIAgentImpl*>(self);
    if (a) {
        a->velX = x;
        a->velY = y;
        a->velZ = z;
    }
}

void bro_AIAgent_setGoal(void* self, double x, double y, double z) {
    auto* a = static_cast<BroAIAgentImpl*>(self);
    if (a) {
        a->goalX = x;
        a->goalY = y;
        a->goalZ = z;
    }
}

void bro_AIAgent_stop(void* self) {
    auto* a = static_cast<BroAIAgentImpl*>(self);
    if (a) {
        a->stopped = true;
        a->velX = 0.0;
        a->velY = 0.0;
        a->velZ = 0.0;
    }
}

// --- Interface bro.ai.AIWorld ---
void* bro_AIWorld_create(void) {
    const auto* b = bro_get_ai_bridge();
    if (b && b->createWorld) {
        return b->createWorld(nullptr);
    }
    return new BroAIWorldImpl();
}

void bro_AIWorld_destroy(void* self) {
    delete static_cast<BroAIWorldImpl*>(self);
}

void bro_AIWorld_step(void* self, double dt) {
    const auto* b = bro_get_ai_bridge();
    if (b && b->step) {
        b->step(self, dt);
        return;
    }
    auto* w = static_cast<BroAIWorldImpl*>(self);
    if (w) {
        w->lastDt = dt;
    }
}

void* bro_AIWorld_createAgent(void* self, void* opts) {
    const auto* b = bro_get_ai_bridge();
    if (b && b->createAgent) {
        return b->createAgent(self, opts);
    }
    auto* w = static_cast<BroAIWorldImpl*>(self);
    auto* a = new BroAIAgentImpl();
    if (w) {
        a->id = w->nextAgentId++;
        w->agents.push_back(a);
    }
    return a;
}

void bro_AIWorld_destroyAgent(void* self, void* agent) {
    auto* w = static_cast<BroAIWorldImpl*>(self);
    auto* a = static_cast<BroAIAgentImpl*>(agent);
    if (w && a) {
        for (auto it = w->agents.begin(); it != w->agents.end(); ++it) {
            if (*it == a) {
                w->agents.erase(it);
                break;
            }
        }
    }
    delete a;
}

// --- Interface bro.ai.AINavGrid ---
void* bro_AINavGrid_create(void) {
    return new BroAINavGridImpl();
}

void bro_AINavGrid_destroy(void* self) {
    delete static_cast<BroAINavGridImpl*>(self);
}

bool bro_AINavGrid_isWalkable(void* self, double x, double z) {
    auto* g = static_cast<BroAINavGridImpl*>(self);
    if (!g) return false;
    int64_t ix = static_cast<int64_t>(x / (g->cellSize > 0 ? g->cellSize : 1.0));
    int64_t iz = static_cast<int64_t>(z / (g->cellSize > 0 ? g->cellSize : 1.0));
    int64_t key = (ix << 32) ^ (iz & 0xFFFFFFFFLL);
    auto it = g->unwalkable.find(key);
    return it == g->unwalkable.end() || !it->second;
}

void bro_AINavGrid_setWalkable(void* self, double x, double z, bool walkable) {
    auto* g = static_cast<BroAINavGridImpl*>(self);
    if (!g) return;
    int64_t ix = static_cast<int64_t>(x / (g->cellSize > 0 ? g->cellSize : 1.0));
    int64_t iz = static_cast<int64_t>(z / (g->cellSize > 0 ? g->cellSize : 1.0));
    int64_t key = (ix << 32) ^ (iz & 0xFFFFFFFFLL);
    g->unwalkable[key] = !walkable;
}

void* bro_AINavGrid_findPath(void* /*self*/, double /*startX*/, double /*startZ*/, double /*endX*/, double /*endZ*/) {
    return nullptr;
}

// --- Interface bro.ai.AINavMesh ---
void* bro_AINavMesh_create(void) {
    return new BroAINavMeshImpl();
}

void bro_AINavMesh_destroy(void* self) {
    delete static_cast<BroAINavMeshImpl*>(self);
}

void* bro_AINavMesh_findPath(void* /*self*/, double /*startX*/, double /*startY*/, double /*startZ*/, double /*endX*/, double /*endY*/, double /*endZ*/) {
    return nullptr;
}

// --- Interface bro.ai.AIMcts ---
void* bro_AIMcts_create(void) {
    return new BroAIMctsImpl();
}

void bro_AIMcts_destroy(void* self) {
    delete static_cast<BroAIMctsImpl*>(self);
}

void* bro_AIMcts_search(void* /*self*/, void* /*state*/, void* /*opts*/) {
    return nullptr;
}

// --- Interface bro.ai.CombatAction ---
void* bro_CombatAction_create(void) {
    return new BroCombatActionImpl();
}

void bro_CombatAction_destroy(void* self) {
    delete static_cast<BroCombatActionImpl*>(self);
}

double bro_CombatAction_get_targetX(void* self) {
    auto* c = static_cast<BroCombatActionImpl*>(self);
    return c ? c->targetX : 0.0;
}

void bro_CombatAction_set_targetX(void* self, double val) {
    auto* c = static_cast<BroCombatActionImpl*>(self);
    if (c) c->targetX = val;
}

double bro_CombatAction_get_targetZ(void* self) {
    auto* c = static_cast<BroCombatActionImpl*>(self);
    return c ? c->targetZ : 0.0;
}

void bro_CombatAction_set_targetZ(void* self, double val) {
    auto* c = static_cast<BroCombatActionImpl*>(self);
    if (c) c->targetZ = val;
}

int32_t bro_CombatAction_get_actionType(void* self) {
    auto* c = static_cast<BroCombatActionImpl*>(self);
    return c ? c->actionType : 0;
}

void bro_CombatAction_set_actionType(void* self, int32_t val) {
    auto* c = static_cast<BroCombatActionImpl*>(self);
    if (c) c->actionType = val;
}

// --- Interface bro.ai.Formation ---
void* bro_Formation_create(void) {
    return new BroFormationImpl();
}

void bro_Formation_destroy(void* self) {
    delete static_cast<BroFormationImpl*>(self);
}

void bro_Formation_setLeader(void* self, void* agent) {
    auto* f = static_cast<BroFormationImpl*>(self);
    if (f) f->leader = agent;
}

void bro_Formation_addFollower(void* self, void* agent, double /*offsetX*/, double /*offsetZ*/) {
    auto* f = static_cast<BroFormationImpl*>(self);
    if (f && agent) f->followers.push_back(agent);
}

void bro_Formation_update(void* /*self*/, double /*dt*/) {
}

// --- Interface bro.ai.VecSim ---
void* bro_VecSim_create(void) {
    return new BroVecSimImpl();
}

void bro_VecSim_destroy(void* self) {
    delete static_cast<BroVecSimImpl*>(self);
}

void bro_VecSim_step(void* self, double dt) {
    auto* v = static_cast<BroVecSimImpl*>(self);
    if (v) v->lastDt = dt;
}

// --- Namespace bro.game ---
void* bro_game_createAgent(void* world, void* opts) {
    if (world) {
        return bro_AIWorld_createAgent(world, opts);
    }
    return new BroAIAgentImpl();
}

void* bro_game_createWorld(void* /*opts*/) {
    return bro_AIWorld_create();
}

void* bro_game_createNavGrid(double minX, double minZ, double maxX, double maxZ, double cellSize) {
    auto* g = new BroAINavGridImpl();
    g->minX = minX;
    g->minZ = minZ;
    g->maxX = maxX;
    g->maxZ = maxZ;
    g->cellSize = cellSize;
    return g;
}

void* bro_game_createNavMesh(void* /*opts*/) {
    return new BroAINavMeshImpl();
}

void* bro_game_createMcts(void* /*config*/) {
    return new BroAIMctsImpl();
}

void* bro_game_createCombatAction(void) {
    return new BroCombatActionImpl();
}

void* bro_game_createFormation(void* /*opts*/) {
    return new BroFormationImpl();
}

void* bro_game_createVecSim(void* /*opts*/) {
    return new BroVecSimImpl();
}

void bro_game_registerCapability(const char* /*name*/, void* /*definition*/) {
}

}
