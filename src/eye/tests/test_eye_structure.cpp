#include "eye/eye_structure.h"

#include "scene/nodes/instanced_mesh_node.h"
#include "scene/nodes/mesh_node.h"
#include "scene/scene_graph.h"
#include <bromesh/manipulation/merge.h>
#include <bromesh/manipulation/transform.h>
#include <bromesh/primitives/primitives.h>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

int gFailures = 0;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", msg);
        ++gFailures;
    }
}

bromesh::MeshData makeInvertedBox(float hw, float hh, float hd) {
    bromesh::MeshData md = bromesh::box(hw, hh, hd);
    for (size_t t = 0; t + 2 < md.indices.size(); t += 3) {
        std::swap(md.indices[t + 1], md.indices[t + 2]);
    }
    return md;
}

bromesh::MeshData makeHollowDoorwayWall(float totalW, float totalH, float thick, float doorW, float doorH) {
    const float jambW = std::max(0.2f, 0.5f * (totalW - doorW));
    const float halfT = thick * 0.5f;
    bromesh::MeshData leftJamb = bromesh::box(jambW * 0.5f, totalH * 0.5f, halfT);
    bromesh::translateMesh(leftJamb, -(doorW * 0.5f + jambW * 0.5f), totalH * 0.5f, 0.0f);

    bromesh::MeshData rightJamb = bromesh::box(jambW * 0.5f, totalH * 0.5f, halfT);
    bromesh::translateMesh(rightJamb, (doorW * 0.5f + jambW * 0.5f), totalH * 0.5f, 0.0f);

    const float lintelH = std::max(0.2f, totalH - doorH);
    bromesh::MeshData lintel = bromesh::box(doorW * 0.5f, lintelH * 0.5f, halfT);
    bromesh::translateMesh(lintel, 0.0f, doorH + lintelH * 0.5f, 0.0f);

    return bromesh::mergeMeshes({leftJamb, rightJamb, lintel});
}

void pushInstance(std::vector<float>& buf, float tx, float ty, float tz, float sx = 1.0f, float sy = 1.0f, float sz = 1.0f) {
    const float rec[16] = {
        sx, 0,  0,  tx,
        0,  sy, 0,  ty,
        0,  0,  sz, tz,
        1,  1,  1,  1,
    };
    buf.insert(buf.end(), rec, rec + 16);
}

void testLegacyBackwardCompat() {
    using namespace bro;
    scene::SceneGraph graph;

    auto* floor = graph.createMesh("Floor");
    floor->setMesh(bromesh::box(4.0f, 0.1f, 4.0f));
    floor->setPosition(0.0f, -0.1f, 0.0f);
    graph.root()->addChild(floor);

    auto* pillar = graph.createMesh("CleanPillar");
    pillar->setMesh(bromesh::box(0.3f, 0.5f, 0.3f));
    pillar->setPosition(-1.5f, 0.5f, 0.0f);
    graph.root()->addChild(pillar);

    auto* inv = graph.createMesh("InvertedBox");
    inv->setMesh(makeInvertedBox(0.3f, 0.3f, 0.3f));
    inv->setPosition(-1.5f, 1.3f, 0.0f);
    graph.root()->addChild(inv);

    auto* flt = graph.createMesh("FloatingCap");
    flt->setMesh(bromesh::box(0.2f, 0.2f, 0.2f));
    flt->setPosition(0.0f, 1.45f, 0.0f);
    graph.root()->addChild(flt);

    auto* ledge = graph.createMesh("Ledge");
    ledge->setMesh(bromesh::box(0.5f, 0.2f, 0.5f));
    ledge->setPosition(1.5f, 0.2f, 0.0f);
    graph.root()->addChild(ledge);

    auto* post = graph.createMesh("OverhangPost");
    post->setMesh(bromesh::box(0.3f, 0.4f, 0.3f));
    post->setPosition(1.5f, 0.8f, 0.50f);
    graph.root()->addChild(post);

    auto* gearA = graph.createMesh("DriverGear");
    gearA->setMesh(bromesh::box(0.2f, 0.2f, 0.1f));
    gearA->setPosition(0.0f, 0.2f, 1.5f);
    graph.root()->addChild(gearA);

    auto* gearB = graph.createMesh("DrivenGear");
    gearB->setMesh(bromesh::box(0.2f, 0.2f, 0.1f));
    gearB->setPosition(0.54f, 0.2f, 1.5f);
    graph.root()->addChild(gearB);

    eye::EyeStructureOptions opts;
    opts.watchPairs.emplace_back("DriverGear", "DrivenGear");
    const auto res = eye::analyzeStructure(graph, opts);

    check(!res.ok, "legacy: detects structural defects");
    check(res.instancedMeshCount == 0, "legacy: zero instanced meshes");
    check(res.invertedCount >= 1, "legacy: detects inverted winding");
    check(res.floatingCount >= 1, "legacy: detects floating node");
    check(res.overhangCount >= 1, "legacy: detects overhang post");
    check(res.gapCount >= 1, "legacy: detects watchPair gap");
}

void testInstancedAndSpatialFilters() {
    using namespace bro;
    scene::SceneGraph graph;

    auto* floor = graph.createMesh("Floor");
    floor->setMesh(bromesh::box(25.0f, 0.1f, 25.0f));
    floor->setPosition(0.0f, -0.1f, 0.0f);
    graph.root()->addChild(floor);

    auto* pillars = graph.createInstancedMesh("InstPillars");
    pillars->setMesh(bromesh::box(0.3f, 0.5f, 0.3f));
    std::vector<float> data;
    pushInstance(data, -2.0f, 0.5f, 0.0f);               // #0 grounded near origin
    pushInstance(data,  0.0f, 2.0f, 0.0f);               // #1 floating near origin
    pushInstance(data, 15.0f, 3.0f, 0.0f);               // #2 floating far away
    pushInstance(data,  2.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f); // #3 zero-scale pool dummy
    pushInstance(data,  4.0f, 0.5f, 0.0f, -1.0f, 1.0f, 1.0f); // #4 negative-det mirror grounded
    pillars->setInstances(data.data(), 5);
    graph.root()->addChild(pillars);

    // Near filter isolates local instances (#0, #1, #4 active; #3 zero-scale skipped; #2 out of range)
    eye::EyeStructureOptions nearOpts;
    nearOpts.hasNear = true;
    nearOpts.nearCenter[0] = 0.0; nearOpts.nearCenter[1] = 0.0; nearOpts.nearCenter[2] = 0.0;
    nearOpts.nearRadius = 6.0;
    const auto nearRes = eye::analyzeStructure(graph, nearOpts);
    check(nearRes.instancedMeshCount == 3, "instanced near: skips zero-scale and far instance (expect 3)");
    check(nearRes.floatingCount == 1, "instanced near: detects exactly 1 floating local instance");

    // Bounds filter covering only grounded #0
    eye::EyeStructureOptions bndOpts;
    bndOpts.hasBounds = true;
    bndOpts.boundsMin[0] = -3.0; bndOpts.boundsMin[1] = -0.5; bndOpts.boundsMin[2] = -1.0;
    bndOpts.boundsMax[0] = -1.0; bndOpts.boundsMax[1] =  2.0; bndOpts.boundsMax[2] =  1.0;
    const auto bndRes = eye::analyzeStructure(graph, bndOpts);
    check(bndRes.ok && bndRes.instancedMeshCount == 1, "instanced bounds: single grounded instance passes cleanly");

    // Inverted instanced mesh
    scene::SceneGraph invGraph;
    auto* fl2 = invGraph.createMesh("Floor");
    fl2->setMesh(bromesh::box(6.0f, 0.1f, 6.0f));
    fl2->setPosition(0.0f, -0.1f, 0.0f);
    invGraph.root()->addChild(fl2);

    auto* invInst = invGraph.createInstancedMesh("BadInstBox");
    invInst->setMesh(makeInvertedBox(0.3f, 0.3f, 0.3f));
    std::vector<float> invData;
    pushInstance(invData, 0.0f, 0.3f, 0.0f);
    invInst->setInstances(invData.data(), 1);
    invGraph.root()->addChild(invInst);
    const auto invRes = eye::analyzeStructure(invGraph);
    check(!invRes.ok && invRes.invertedCount == 1, "instanced: detects inverted winding on InstancedMeshNode");

    // Non-uniformly Y-scaled unit boxes (sy=0.05 ledge supporting sy=0.1 overhanging post)
    scene::SceneGraph scaledGraph;
    auto* sGround = scaledGraph.createMesh("Floor");
    sGround->setMesh(bromesh::box(6.0f, 0.1f, 6.0f));
    sGround->setPosition(0.0f, -0.1f, 0.0f);
    scaledGraph.root()->addChild(sGround);

    auto* sLedge = scaledGraph.createInstancedMesh("ScaledLedge");
    sLedge->setMesh(bromesh::box(0.5f, 0.5f, 0.5f));
    std::vector<float> sLedgeData;
    pushInstance(sLedgeData, 0.0f, 1.0f, 0.0f, 4.0f, 0.05f, 4.0f); // y in [0.975, 1.025], z in [-2, 2]
    sLedge->setInstances(sLedgeData.data(), 1);
    scaledGraph.root()->addChild(sLedge);

    auto* sPost = scaledGraph.createInstancedMesh("ScaledOverhangPost");
    sPost->setMesh(bromesh::box(0.5f, 1.0f, 0.5f));
    std::vector<float> sPostData;
    pushInstance(sPostData, 0.0f, 1.125f, 2.0f, 0.3f, 0.1f, 0.3f); // y in [1.025, 1.225], z in [1.85, 2.15] (50% off ledge)
    sPost->setInstances(sPostData.data(), 1);
    scaledGraph.root()->addChild(sPost);

    eye::EyeStructureOptions sOpts;
    sOpts.ignoreFloating.push_back("ScaledLedge");
    const auto sRes = eye::analyzeStructure(scaledGraph, sOpts);
    check(!sRes.ok && sRes.overhangCount >= 1, "instanced scaled: catches overhang on sy=0.05 ledge + sy=0.1 post");
}

void testMultiFloorSlabsAndFootings() {
    using namespace bro;
    scene::SceneGraph graph;

    // 3-story tower: ground y=0, floor 1 y=3.5 (with stairwell opening on +X), floor 2 y=7.0
    auto* f0 = graph.createMesh("Floor_Ground");
    f0->setMesh(bromesh::box(4.0f, 0.1f, 4.0f));
    f0->setPosition(0.0f, -0.1f, 0.0f);
    graph.root()->addChild(f0);

    auto* f1 = graph.createMesh("Floor1_Slab");
    f1->setMesh(bromesh::box(2.0f, 0.1f, 4.0f)); // x in [-2, 2], z in [-4, 4], top at y=3.5
    f1->setPosition(0.0f, 3.4f, 0.0f);
    graph.root()->addChild(f1);

    auto* f2 = graph.createMesh("Floor2_Slab");
    f2->setMesh(bromesh::box(4.0f, 0.1f, 4.0f)); // top at y=7.0
    f2->setPosition(0.0f, 6.9f, 0.0f);
    graph.root()->addChild(f2);

    // Valid chair on floor 1 (at y=3.5..4.3, x=0)
    auto* okChair = graph.createMesh("Chair_F1");
    okChair->setMesh(bromesh::box(0.3f, 0.4f, 0.3f));
    okChair->setPosition(0.0f, 3.9f, 0.0f);
    graph.root()->addChild(okChair);

    // Floating prop on floor 1 (hovering 0.35m above y=3.5)
    auto* floatProp = graph.createMesh("FloatingChair_F1");
    floatProp->setMesh(bromesh::box(0.3f, 0.3f, 0.3f));
    floatProp->setPosition(-1.0f, 4.15f, 0.0f);
    graph.root()->addChild(floatProp);

    // Overhanging table on floor 1 edge (x centered at 2.0, half-width 0.6 -> x in [1.4, 2.6], 50% off slab!)
    auto* overhangTbl = graph.createMesh("OverhangTable_F1");
    overhangTbl->setMesh(bromesh::box(0.6f, 0.4f, 0.4f));
    overhangTbl->setPosition(2.05f, 3.9f, 0.0f);
    graph.root()->addChild(overhangTbl);

    // Wide flat table (2.0 x 0.3 x 1.6 m) resting at y=3.5, 45% overhanging stairwell cutout
    auto* wideTbl = graph.createMesh("WideCutoutTable_F1");
    wideTbl->setMesh(bromesh::box(1.0f, 0.15f, 0.8f));
    wideTbl->setPosition(1.9f, 3.65f, 1.5f); // x in [0.9, 2.9], y in [3.5, 3.8]
    graph.root()->addChild(wideTbl);

    eye::EyeStructureOptions opts;
    opts.autoDetectFloors = true;
    opts.minFloorSeparation = 2.0;
    const auto res = eye::analyzeStructure(graph, opts);

    check(res.floors.size() == 3, "multifloor: auto-detected 3 floor slabs (ignoring wide table)");
    check(res.floorDefectCount == 0, "multifloor: valid 3.5m floor separation has 0 separation defects");
    check(res.floatingCount == 1, "multifloor: detects floating prop on upper floor");
    check(res.overhangCount == 2, "multifloor: detects overhanging table and wide cutout table under autoDetectFloors");

    eye::EyeStructureOptions expOpts;
    expOpts.floors.push_back({0.0, 0.05, "Ground"});
    expOpts.floors.push_back({3.5, 0.05, "Floor1"});
    expOpts.floors.push_back({7.0, 0.05, "Floor2"});
    expOpts.minFloorSeparation = 2.0;
    const auto expRes = eye::analyzeStructure(graph, expOpts);
    check(expRes.floors.size() == 3 && expRes.floors[1].slabCount == 1, "multifloor explicit: wide table is NOT counted as floor slab");
    check(expRes.overhangCount == 2, "multifloor explicit: catches overhanging wide cutout table");

    // Overlapping floor separation defect check
    auto* badMezz = graph.createMesh("Mezzanine_Slab");
    badMezz->setMesh(bromesh::box(2.0f, 0.1f, 2.0f));
    badMezz->setPosition(0.0f, 4.3f, 0.0f); // top at 4.4 vs floor 1 at 3.5 -> sep 0.9m < 2.0m
    graph.root()->addChild(badMezz);
    const auto sepRes = eye::analyzeStructure(graph, opts);
    check(sepRes.floorDefectCount >= 1, "multifloor: detects insufficient floor-to-floor separation");
}

void testInteriorWalkability() {
    using namespace bro;
    scene::SceneGraph graph;

    auto* ground = graph.createMesh("Floor_Ground");
    ground->setMesh(bromesh::box(14.0f, 0.1f, 8.0f));
    ground->setPosition(0.0f, -0.1f, 0.0f);
    graph.root()->addChild(ground);

    // Hollow doorway wall at z = 0 with 1.2m wide x 2.2m tall cutout centered at x=0
    auto* doorWall = graph.createMesh("DoorWall");
    doorWall->setMesh(makeHollowDoorwayWall(4.0f, 3.0f, 0.25f, 1.2f, 2.2f));
    doorWall->setPosition(0.0f, 0.0f, 0.0f);
    graph.root()->addChild(doorWall);

    // Valid stair flight at x=3, z from 0 to 3.0, y from 0 to 1.5 (10 treads, 0.15m rise each)
    auto* stairs = graph.createInstancedMesh("Stairs");
    stairs->setMesh(bromesh::box(0.6f, 0.08f, 0.18f));
    std::vector<float> sData;
    for (int k = 0; k < 10; ++k) {
        const float ty = 0.15f * (k + 1) - 0.08f;
        const float tz = 0.30f * k + 0.15f;
        pushInstance(sData, 3.0f, ty, tz);
    }
    stairs->setInstances(sData.data(), 10);
    graph.root()->addChild(stairs);

    // Valid residential stair (rise=0.20, run=0.24, stepUp=0.30) at x=6.0
    auto* resStairs = graph.createInstancedMesh("ResidentialStairs");
    resStairs->setMesh(bromesh::box(0.5f, 0.11f, 0.14f));
    std::vector<float> rsData;
    for (int k = 0; k < 6; ++k) {
        const float ty = 0.20f * (k + 1) - 0.11f;
        const float tz = 0.24f * k + 0.12f;
        pushInstance(rsData, 6.0f, ty, tz);
    }
    resStairs->setInstances(rsData.data(), 6);
    graph.root()->addChild(resStairs);

    eye::EyeStructureOptions opts;
    opts.interiorWalk.enabled = true;
    opts.interiorWalk.stepUp = 0.30;
    opts.interiorWalk.doorways.push_back({"OpenDoor", {0.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, 1.2, 2.2, 0.8});
    opts.interiorWalk.corridors.push_back({"EntryHall", {-2.0, 0.0, -1.5}, {2.0, 0.0, -1.5}, 1.4, 2.5});
    opts.interiorWalk.stairs.push_back({"MainStair", {3.0, 0.15, 0.15}, {3.0, 1.50, 2.85}, 1.2, 2.2});
    opts.interiorWalk.stairs.push_back({"ResidentialStair", {6.0, 0.20, 0.12}, {6.0, 1.20, 1.32}, 1.0, 2.2});

    const auto okRes = eye::analyzeStructure(graph, opts);
    check(okRes.ok, "walk: clean scene with hollow doorway, corridor, and residential stair passes");
    check(okRes.interiorWalk.checked, "walk: interiorWalk checked");
    check(okRes.interiorWalk.doorwaysBlocked == 0, "walk: hollow doorway cutout is NOT falsely blocked");
    check(okRes.interiorWalk.corridorsBlocked == 0, "walk: open corridor passes");
    check(okRes.interiorWalk.stairsBroken == 0, "walk: valid residential stair flight passes");
    check(okRes.interiorWalk.walkIssueCount == 0, "walk: zero walk issues on valid interior");

    // Add blocking crate inside doorway, low beam in corridor, and narrow doorway
    auto* crate = graph.createMesh("BlockingCrate");
    crate->setMesh(bromesh::box(0.3f, 0.6f, 0.3f));
    crate->setPosition(0.0f, 0.6f, 0.0f);
    graph.root()->addChild(crate);

    auto* lowBeam = graph.createMesh("LowBeam");
    lowBeam->setMesh(bromesh::box(0.4f, 0.2f, 0.6f));
    lowBeam->setPosition(0.0f, 2.08f, -1.5f); // bottom at 1.88m: > agentHeight 1.85m, < minHeadroom 1.95m
    graph.root()->addChild(lowBeam);

    opts.interiorWalk.stepUp = 0.40;
    opts.interiorWalk.doorways.push_back({"NarrowDoor", {-2.0, 0.0, 2.0}, {0.0, 0.0, 1.0}, 0.5, 2.1, 0.8});

    // Steep wide-tread stair (rise=0.55 > stepUp=0.40, run=0.90) -> must emit stair-step-too-high
    auto* steepStairs = graph.createInstancedMesh("SteepWideStairs");
    steepStairs->setMesh(bromesh::box(0.5f, 0.05f, 0.45f));
    std::vector<float> swData;
    for (int k = 0; k < 3; ++k) {
        const float ty = 0.55f * (k + 1) - 0.05f;
        const float tz = 0.90f * k + 0.45f;
        pushInstance(swData, -6.0f, ty, tz);
    }
    steepStairs->setInstances(swData.data(), 3);
    graph.root()->addChild(steepStairs);
    opts.interiorWalk.stairs.push_back({"SteepWideStair", {-6.0, 0.55, 0.45}, {-6.0, 1.65, 2.25}, 1.0, 2.2});

    // Low-headroom stair (beam at y=2.4 above stair) -> must emit stair-low-headroom
    auto* headStairs = graph.createInstancedMesh("HeadroomStairs");
    headStairs->setMesh(bromesh::box(0.6f, 0.08f, 0.18f));
    std::vector<float> hsData;
    for (int k = 0; k < 10; ++k) {
        pushInstance(hsData, -9.0f, 0.15f * (k + 1) - 0.08f, 0.30f * k + 0.15f);
    }
    headStairs->setInstances(hsData.data(), 10);
    graph.root()->addChild(headStairs);
    auto* stairBeam = graph.createMesh("StairBeam");
    stairBeam->setMesh(bromesh::box(0.6f, 0.2f, 0.3f));
    stairBeam->setPosition(-9.0f, 2.4f, 1.5f); // bottom at 2.2m above tread ~0.75m -> clearH 1.45m < 2.2m
    graph.root()->addChild(stairBeam);
    opts.interiorWalk.stairs.push_back({"HeadroomStair", {-9.0, 0.15, 0.15}, {-9.0, 1.50, 2.85}, 1.2, 2.2});

    // Missing low stair tread (k=2 at y=0.45m above ground y=0) -> must emit stair-gap
    auto* gapStairs = graph.createInstancedMesh("GapStairsLow");
    gapStairs->setMesh(bromesh::box(0.6f, 0.08f, 0.18f));
    std::vector<float> gsData;
    for (int k = 0; k < 10; ++k) {
        if (k == 2) continue;
        pushInstance(gsData, 9.0f, 0.15f * (k + 1) - 0.08f, 0.30f * k + 0.15f);
    }
    gapStairs->setInstances(gsData.data(), 9);
    graph.root()->addChild(gapStairs);
    opts.interiorWalk.stairs.push_back({"GapStairLow", {9.0, 0.15, 0.15}, {9.0, 1.50, 2.85}, 1.2, 2.2});

    const auto badRes = eye::analyzeStructure(graph, opts);
    auto hasWalkIssue = [&](const std::string& kind, const std::string& nodeA) {
        for (const auto& iss : badRes.issues) {
            if (iss.kind == kind && iss.nodeA == nodeA) return true;
        }
        return false;
    };
    check(badRes.interiorWalk.doorwaysBlocked == 2, "walk: catches blocked doorway and narrow doorway");
    check(badRes.interiorWalk.corridorsBlocked == 1, "walk: catches low headroom beam in corridor");
    check(badRes.interiorWalk.headroomIssues >= 2, "walk: records corridor and stair headroom issues");
    check(badRes.interiorWalk.stairsBroken == 3, "walk: catches steep, low-headroom, and low-gap stairs");
    check(hasWalkIssue("stair-step-too-high", "SteepWideStair"), "walk: steep wide stair emits stair-step-too-high");
    check(hasWalkIssue("stair-low-headroom", "HeadroomStair"), "walk: low-headroom stair emits stair-low-headroom");
    check(hasWalkIssue("stair-gap", "GapStairLow"), "walk: missing low tread emits stair-gap");
}

void testGroundGridHilltop() {
    bro::scene::SceneGraph graph;
    auto* hillBox = graph.createMesh("HillPost");
    hillBox->setMesh(bromesh::box(0.5f, 1.0f, 0.5f));
    hillBox->setPosition(5.0f, 11.0f, 5.0f); // bottom at y = 10.0 on a 10 m hill
    graph.root()->addChild(hillBox);

    auto* floatingBox = graph.createMesh("FloatingAboveHill");
    floatingBox->setMesh(bromesh::box(0.5f, 1.0f, 0.5f));
    floatingBox->setPosition(-5.0f, 11.5f, -5.0f); // bottom at y = 10.5 (0.5 m above 10 m hill)
    graph.root()->addChild(floatingBox);

    bro::eye::EyeStructureOptions opts;
    opts.groundY = 0.0;
    opts.groundGrid.originX = -10.0;
    opts.groundGrid.originZ = -10.0;
    opts.groundGrid.step = 5.0;
    opts.groundGrid.cols = 5;
    opts.groundGrid.rows = 5;
    opts.groundGrid.heights.assign(25, 10.0f);

    const auto res = bro::eye::analyzeStructure(graph, opts);
    check(res.floatingCount == 1, "groundGrid: hill-seated box grounded while 0.5 m air gap is caught");
}

}  // namespace

int main() {
    testLegacyBackwardCompat();
    testInstancedAndSpatialFilters();
    testMultiFloorSlabsAndFootings();
    testInteriorWalkability();
    testGroundGridHilltop();
    if (gFailures > 0) {
        std::fprintf(stderr, "FAIL: bro_eye_structure_test (%d failures)\n", gFailures);
        return 1;
    }
    std::printf("PASS: bro_eye_structure_test (all assertions passed)\n");
    return 0;
}
