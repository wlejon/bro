#include "js/ai_bindings.h"
#include "util/log.h"

#include <brogameagent/brogameagent.h>

extern "C" {
#include "quickjs.h"
}

#include <memory>
#include <unordered_map>
#include <vector>

namespace bro::js {

// ─── Opaque pointers stored on JS objects ───────────────────────────────────

static JSClassID s_navGridClassId = 0;
static JSClassID s_agentClassId = 0;

// ─── NavGrid class ──────────────────────────────────────────────────────────

static void js_navgrid_finalizer(JSRuntime*, JSValue val) {
    auto* grid = static_cast<brogameagent::NavGrid*>(JS_GetOpaque(val, s_navGridClassId));
    delete grid;
}

static JSClassDef s_navGridClassDef = {
    "NavGrid", js_navgrid_finalizer, nullptr, nullptr, nullptr
};

// bro.ai.game.createNavGrid({ minX, minZ, maxX, maxZ, cellSize })
static JSValue js_ai_createNavGrid(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 1 || !JS_IsObject(argv[0]))
        return JS_ThrowTypeError(ctx, "createNavGrid() requires an options object");

    JSValue opts = argv[0];
    double minX = -20, minZ = -20, maxX = 20, maxZ = 20, cellSize = 0.5;

    JSValue v;
    v = JS_GetPropertyStr(ctx, opts, "minX"); if (JS_IsNumber(v)) JS_ToFloat64(ctx, &minX, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "minZ"); if (JS_IsNumber(v)) JS_ToFloat64(ctx, &minZ, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "maxX"); if (JS_IsNumber(v)) JS_ToFloat64(ctx, &maxX, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "maxZ"); if (JS_IsNumber(v)) JS_ToFloat64(ctx, &maxZ, v); JS_FreeValue(ctx, v);
    v = JS_GetPropertyStr(ctx, opts, "cellSize"); if (JS_IsNumber(v)) JS_ToFloat64(ctx, &cellSize, v); JS_FreeValue(ctx, v);

    auto* grid = new brogameagent::NavGrid(
        static_cast<float>(minX), static_cast<float>(minZ),
        static_cast<float>(maxX), static_cast<float>(maxZ),
        static_cast<float>(cellSize));

    // Process obstacles array if provided
    v = JS_GetPropertyStr(ctx, opts, "obstacles");
    if (JS_IsArray(v)) {
        JSValue lenVal = JS_GetPropertyStr(ctx, v, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, lenVal);
        JS_FreeValue(ctx, lenVal);

        double padding = 0;
        JSValue padVal = JS_GetPropertyStr(ctx, opts, "padding");
        if (JS_IsNumber(padVal)) JS_ToFloat64(ctx, &padding, padVal);
        JS_FreeValue(ctx, padVal);

        for (int32_t i = 0; i < len; i++) {
            JSValue ob = JS_GetPropertyUint32(ctx, v, i);
            double ox = 0, oz = 0, ohw = 0, ohd = 0;
            JSValue tmp;
            tmp = JS_GetPropertyStr(ctx, ob, "x"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &ox, tmp); JS_FreeValue(ctx, tmp);
            tmp = JS_GetPropertyStr(ctx, ob, "z"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &oz, tmp); JS_FreeValue(ctx, tmp);
            tmp = JS_GetPropertyStr(ctx, ob, "hw"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &ohw, tmp); JS_FreeValue(ctx, tmp);
            tmp = JS_GetPropertyStr(ctx, ob, "hd"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &ohd, tmp); JS_FreeValue(ctx, tmp);
            JS_FreeValue(ctx, ob);

            grid->addObstacle(
                {static_cast<float>(ox), static_cast<float>(oz),
                 static_cast<float>(ohw), static_cast<float>(ohd)},
                static_cast<float>(padding));
        }
    }
    JS_FreeValue(ctx, v);

    JSValue obj = JS_NewObjectClass(ctx, s_navGridClassId);
    JS_SetOpaque(obj, grid);
    return obj;
}

// navGrid.isWalkable(x, z) → boolean
static JSValue js_navgrid_isWalkable(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* grid = static_cast<brogameagent::NavGrid*>(JS_GetOpaque(this_val, s_navGridClassId));
    if (!grid || argc < 2) return JS_FALSE;

    double x, z;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &z, argv[1]);

    return JS_NewBool(ctx, grid->isWalkable(static_cast<float>(x), static_cast<float>(z)));
}

// navGrid.findPath(fromX, fromZ, toX, toZ) → [{x, z}, ...]
static JSValue js_navgrid_findPath(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* grid = static_cast<brogameagent::NavGrid*>(JS_GetOpaque(this_val, s_navGridClassId));
    if (!grid || argc < 4) return JS_NewArray(ctx);

    double fx, fz, tx, tz;
    JS_ToFloat64(ctx, &fx, argv[0]);
    JS_ToFloat64(ctx, &fz, argv[1]);
    JS_ToFloat64(ctx, &tx, argv[2]);
    JS_ToFloat64(ctx, &tz, argv[3]);

    auto path = grid->findPath(
        {static_cast<float>(fx), static_cast<float>(fz)},
        {static_cast<float>(tx), static_cast<float>(tz)});

    JSValue arr = JS_NewArray(ctx);
    for (size_t i = 0; i < path.size(); i++) {
        JSValue pt = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, pt, "x", JS_NewFloat64(ctx, path[i].x));
        JS_SetPropertyStr(ctx, pt, "z", JS_NewFloat64(ctx, path[i].z));
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i), pt);
    }
    return arr;
}

// navGrid.addObstacle({ x, z, hw, hd }, padding?)
static JSValue js_navgrid_addObstacle(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* grid = static_cast<brogameagent::NavGrid*>(JS_GetOpaque(this_val, s_navGridClassId));
    if (!grid || argc < 1 || !JS_IsObject(argv[0])) return JS_UNDEFINED;

    double ox = 0, oz = 0, ohw = 0, ohd = 0, padding = 0;
    JSValue tmp;
    tmp = JS_GetPropertyStr(ctx, argv[0], "x"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &ox, tmp); JS_FreeValue(ctx, tmp);
    tmp = JS_GetPropertyStr(ctx, argv[0], "z"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &oz, tmp); JS_FreeValue(ctx, tmp);
    tmp = JS_GetPropertyStr(ctx, argv[0], "hw"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &ohw, tmp); JS_FreeValue(ctx, tmp);
    tmp = JS_GetPropertyStr(ctx, argv[0], "hd"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &ohd, tmp); JS_FreeValue(ctx, tmp);

    if (argc >= 2) JS_ToFloat64(ctx, &padding, argv[1]);

    grid->addObstacle(
        {static_cast<float>(ox), static_cast<float>(oz),
         static_cast<float>(ohw), static_cast<float>(ohd)},
        static_cast<float>(padding));
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry js_navgrid_proto[] = {
    JS_CFUNC_DEF("isWalkable", 2, js_navgrid_isWalkable),
    JS_CFUNC_DEF("findPath", 4, js_navgrid_findPath),
    JS_CFUNC_DEF("addObstacle", 2, js_navgrid_addObstacle),
};

// ─── Agent class ─────────────────────────────────────────────────────────────

struct AgentHandle {
    brogameagent::Agent agent;
    const brogameagent::NavGrid* gridRef = nullptr; // prevent GC? no, tracked via JS ref
};

static void js_agent_finalizer(JSRuntime*, JSValue val) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(val, s_agentClassId));
    delete h;
}

static JSClassDef s_agentClassDef = {
    "Agent", js_agent_finalizer, nullptr, nullptr, nullptr
};

// bro.ai.game.createAgent({ navGrid?, x, z, speed, radius })
static JSValue js_ai_createAgent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* h = new AgentHandle();

    if (argc >= 1 && JS_IsObject(argv[0])) {
        JSValue opts = argv[0];
        double x = 0, z = 0, speed = 6.0, radius = 0.4;
        JSValue v;

        v = JS_GetPropertyStr(ctx, opts, "x"); if (JS_IsNumber(v)) JS_ToFloat64(ctx, &x, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "z"); if (JS_IsNumber(v)) JS_ToFloat64(ctx, &z, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "speed"); if (JS_IsNumber(v)) JS_ToFloat64(ctx, &speed, v); JS_FreeValue(ctx, v);
        v = JS_GetPropertyStr(ctx, opts, "radius"); if (JS_IsNumber(v)) JS_ToFloat64(ctx, &radius, v); JS_FreeValue(ctx, v);

        h->agent.setPosition(static_cast<float>(x), static_cast<float>(z));
        h->agent.setSpeed(static_cast<float>(speed));
        h->agent.setRadius(static_cast<float>(radius));

        // navGrid — extract the C++ pointer from the JS object
        v = JS_GetPropertyStr(ctx, opts, "navGrid");
        if (JS_IsObject(v)) {
            auto* grid = static_cast<brogameagent::NavGrid*>(JS_GetOpaque2(ctx, v, s_navGridClassId));
            if (grid) {
                h->agent.setNavGrid(grid);
                h->gridRef = grid;
            }
        }
        JS_FreeValue(ctx, v);
    }

    JSValue obj = JS_NewObjectClass(ctx, s_agentClassId);
    JS_SetOpaque(obj, h);
    return obj;
}

// agent.setTarget(x, z)
static JSValue js_agent_setTarget(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    if (!h || argc < 2) return JS_UNDEFINED;
    double x, z;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &z, argv[1]);
    h->agent.setTarget(static_cast<float>(x), static_cast<float>(z));
    return JS_UNDEFINED;
}

// agent.clearTarget()
static JSValue js_agent_clearTarget(JSContext* ctx, JSValueConst this_val, int, JSValueConst*) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    if (h) h->agent.clearTarget();
    return JS_UNDEFINED;
}

// agent.update(dt)
static JSValue js_agent_update(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    if (!h || argc < 1) return JS_UNDEFINED;
    double dt;
    JS_ToFloat64(ctx, &dt, argv[0]);
    h->agent.update(static_cast<float>(dt));
    return JS_UNDEFINED;
}

// agent.setPosition(x, z)
static JSValue js_agent_setPosition(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    if (!h || argc < 2) return JS_UNDEFINED;
    double x, z;
    JS_ToFloat64(ctx, &x, argv[0]);
    JS_ToFloat64(ctx, &z, argv[1]);
    h->agent.setPosition(static_cast<float>(x), static_cast<float>(z));
    return JS_UNDEFINED;
}

// agent.aimAt(tx, ty, tz, eyeHeight) → { yaw, pitch }
static JSValue js_agent_aimAt(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    if (!h || argc < 4) return JS_NULL;
    double tx, ty, tz, eyeH;
    JS_ToFloat64(ctx, &tx, argv[0]);
    JS_ToFloat64(ctx, &ty, argv[1]);
    JS_ToFloat64(ctx, &tz, argv[2]);
    JS_ToFloat64(ctx, &eyeH, argv[3]);
    auto aim = h->agent.aimAt(static_cast<float>(tx), static_cast<float>(ty),
                               static_cast<float>(tz), static_cast<float>(eyeH));
    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "yaw", JS_NewFloat64(ctx, aim.yaw));
    JS_SetPropertyStr(ctx, obj, "pitch", JS_NewFloat64(ctx, aim.pitch));
    return obj;
}

// agent.x, agent.z, agent.yaw, agent.hasTarget, agent.atTarget (getters)
static JSValue js_agent_get_x(JSContext* ctx, JSValueConst this_val) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    return h ? JS_NewFloat64(ctx, h->agent.x()) : JS_NewFloat64(ctx, 0);
}
static JSValue js_agent_get_z(JSContext* ctx, JSValueConst this_val) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    return h ? JS_NewFloat64(ctx, h->agent.z()) : JS_NewFloat64(ctx, 0);
}
static JSValue js_agent_get_yaw(JSContext* ctx, JSValueConst this_val) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    return h ? JS_NewFloat64(ctx, h->agent.yaw()) : JS_NewFloat64(ctx, 0);
}
static JSValue js_agent_get_hasTarget(JSContext* ctx, JSValueConst this_val) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    return JS_NewBool(ctx, h && h->agent.hasTarget());
}
static JSValue js_agent_get_atTarget(JSContext* ctx, JSValueConst this_val) {
    auto* h = static_cast<AgentHandle*>(JS_GetOpaque(this_val, s_agentClassId));
    return JS_NewBool(ctx, h && h->agent.atTarget());
}

static const JSCFunctionListEntry js_agent_proto[] = {
    JS_CFUNC_DEF("setTarget", 2, js_agent_setTarget),
    JS_CFUNC_DEF("clearTarget", 0, js_agent_clearTarget),
    JS_CFUNC_DEF("update", 1, js_agent_update),
    JS_CFUNC_DEF("setPosition", 2, js_agent_setPosition),
    JS_CFUNC_DEF("aimAt", 4, js_agent_aimAt),
    JS_CGETSET_DEF("x", js_agent_get_x, nullptr),
    JS_CGETSET_DEF("z", js_agent_get_z, nullptr),
    JS_CGETSET_DEF("yaw", js_agent_get_yaw, nullptr),
    JS_CGETSET_DEF("hasTarget", js_agent_get_hasTarget, nullptr),
    JS_CGETSET_DEF("atTarget", js_agent_get_atTarget, nullptr),
};

// ─── Standalone functions ────────────────────────────────────────────────────

// bro.ai.game.hasLineOfSight(fromX, fromZ, toX, toZ, obstacles)
// obstacles: [{ x, z, hw, hd }, ...]
static JSValue js_ai_hasLOS(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 5) return JS_FALSE;

    double fx, fz, tx, tz;
    JS_ToFloat64(ctx, &fx, argv[0]);
    JS_ToFloat64(ctx, &fz, argv[1]);
    JS_ToFloat64(ctx, &tx, argv[2]);
    JS_ToFloat64(ctx, &tz, argv[3]);

    if (!JS_IsArray(argv[4])) return JS_FALSE;

    JSValue lenVal = JS_GetPropertyStr(ctx, argv[4], "length");
    int32_t len = 0;
    JS_ToInt32(ctx, &len, lenVal);
    JS_FreeValue(ctx, lenVal);

    std::vector<brogameagent::AABB> boxes;
    boxes.reserve(len);
    for (int32_t i = 0; i < len; i++) {
        JSValue ob = JS_GetPropertyUint32(ctx, argv[4], i);
        double ox = 0, oz = 0, ohw = 0, ohd = 0;
        JSValue tmp;
        tmp = JS_GetPropertyStr(ctx, ob, "x"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &ox, tmp); JS_FreeValue(ctx, tmp);
        tmp = JS_GetPropertyStr(ctx, ob, "z"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &oz, tmp); JS_FreeValue(ctx, tmp);
        tmp = JS_GetPropertyStr(ctx, ob, "hw"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &ohw, tmp); JS_FreeValue(ctx, tmp);
        tmp = JS_GetPropertyStr(ctx, ob, "hd"); if (JS_IsNumber(tmp)) JS_ToFloat64(ctx, &ohd, tmp); JS_FreeValue(ctx, tmp);
        JS_FreeValue(ctx, ob);
        boxes.push_back({static_cast<float>(ox), static_cast<float>(oz),
                         static_cast<float>(ohw), static_cast<float>(ohd)});
    }

    bool los = brogameagent::hasLineOfSight(
        {static_cast<float>(fx), static_cast<float>(fz)},
        {static_cast<float>(tx), static_cast<float>(tz)},
        boxes.data(), static_cast<int>(boxes.size()));

    return JS_NewBool(ctx, los);
}

// bro.ai.game.computeAim(fromX, fromY, fromZ, toX, toY, toZ) → { yaw, pitch }
static JSValue js_ai_computeAim(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 6) return JS_NULL;
    double fx, fy, fz, tx, ty, tz;
    JS_ToFloat64(ctx, &fx, argv[0]);
    JS_ToFloat64(ctx, &fy, argv[1]);
    JS_ToFloat64(ctx, &fz, argv[2]);
    JS_ToFloat64(ctx, &tx, argv[3]);
    JS_ToFloat64(ctx, &ty, argv[4]);
    JS_ToFloat64(ctx, &tz, argv[5]);

    auto aim = brogameagent::computeAim(
        static_cast<float>(fx), static_cast<float>(fy), static_cast<float>(fz),
        static_cast<float>(tx), static_cast<float>(ty), static_cast<float>(tz));

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "yaw", JS_NewFloat64(ctx, aim.yaw));
    JS_SetPropertyStr(ctx, obj, "pitch", JS_NewFloat64(ctx, aim.pitch));
    return obj;
}

// ─── Function list for bro.ai.game ──────────────────────────────────────────

static const JSCFunctionListEntry js_ai_game_funcs[] = {
    JS_CFUNC_DEF("createNavGrid", 1, js_ai_createNavGrid),
    JS_CFUNC_DEF("createAgent", 1, js_ai_createAgent),
    JS_CFUNC_DEF("hasLineOfSight", 5, js_ai_hasLOS),
    JS_CFUNC_DEF("computeAim", 6, js_ai_computeAim),
};

// ─── Install / Cleanup ──────────────────────────────────────────────────────

void AIBindings::install(JSContext* ctx) {
    JSRuntime* rt = JS_GetRuntime(ctx);

    // Register NavGrid class
    JS_NewClassID(rt, &s_navGridClassId);
    JS_NewClass(rt, s_navGridClassId, &s_navGridClassDef);
    JSValue navGridProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, navGridProto, js_navgrid_proto,
                               sizeof(js_navgrid_proto) / sizeof(js_navgrid_proto[0]));
    JS_SetClassProto(ctx, s_navGridClassId, navGridProto);

    // Register Agent class
    JS_NewClassID(rt, &s_agentClassId);
    JS_NewClass(rt, s_agentClassId, &s_agentClassDef);
    JSValue agentProto = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, agentProto, js_agent_proto,
                               sizeof(js_agent_proto) / sizeof(js_agent_proto[0]));
    JS_SetClassProto(ctx, s_agentClassId, agentProto);

    // Create bro.ai.game namespace
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue broObj = JS_GetPropertyStr(ctx, global, "bro");
    if (JS_IsUndefined(broObj) || JS_IsException(broObj)) {
        broObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, global, "bro", JS_DupValue(ctx, broObj));
    }

    JSValue aiObj = JS_GetPropertyStr(ctx, broObj, "ai");
    if (JS_IsUndefined(aiObj) || JS_IsException(aiObj)) {
        aiObj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, broObj, "ai", JS_DupValue(ctx, aiObj));
    }

    JSValue gameObj = JS_NewObject(ctx);
    JS_SetPropertyFunctionList(ctx, gameObj, js_ai_game_funcs,
                               sizeof(js_ai_game_funcs) / sizeof(js_ai_game_funcs[0]));

    JS_SetPropertyStr(ctx, aiObj, "game", gameObj);
    JS_FreeValue(ctx, aiObj);
    JS_FreeValue(ctx, broObj);
    JS_FreeValue(ctx, global);
}

void AIBindings::cleanup(JSContext*) {
    // No persistent state to clean up currently
}

} // namespace bro::js
