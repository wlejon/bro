#if BRO_WITH_3D

#include "bronze_host/host_mesh_internal.h"
#include "bromesh/primitives/primitives.h"
#include "bromesh/primitives/par_primitives.h"
#include "bromesh/manipulation/sweep.h"
#include "bromesh/manipulation/bezier_sweep.h"
#include "bromesh/procedural/plants.h"
#include "bromesh/procedural/branches.h"
#include "bromesh/procedural/leaf_scatter.h"
#include "bromesh/procedural/space_colonization.h"

namespace bro::bronze_host {

namespace {

inline double numAtProp(Value obj, const char* k, double defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, k);
    return ev::isNumber(v) ? ev::toDouble(v) : defVal;
}

inline int intAtProp(Value obj, const char* k, int defVal) {
    return static_cast<int>(numAtProp(obj, k, defVal));
}

inline bool boolAtProp(Value obj, const char* k, bool defVal) {
    if (!ev::isObject(obj)) return defVal;
    Value v = ev::getProperty(obj, k);
    return !ev::isUndefined(v) ? ev::toBool(v) : defVal;
}

inline bromesh::LeafShape parseLeafShape(Value v) {
    if (!ev::isString(v)) return bromesh::LeafShape::Oval;
    std::string s = ev::toUtf8(v);
    if (s == "pointed") return bromesh::LeafShape::Pointed;
    if (s == "lobed")   return bromesh::LeafShape::Lobed;
    if (s == "needle")  return bromesh::LeafShape::Needle;
    if (s == "frond")   return bromesh::LeafShape::Frond;
    if (s == "petal")   return bromesh::LeafShape::Petal;
    return bromesh::LeafShape::Oval;
}

inline void readLeafPlacementOptions(Value o, bromesh::LeafPlacementOptions& opts) {
    opts.maxRadius      = static_cast<float>(numAtProp(o, "maxRadius",       opts.maxRadius));
    opts.minDepth       =                    intAtProp(o, "minDepth",        opts.minDepth);
    opts.terminalOnly   =                   boolAtProp(o, "terminalOnly",    opts.terminalOnly);
    opts.perUnitLength  = static_cast<float>(numAtProp(o, "perUnitLength",   opts.perUnitLength));
    opts.densityFalloff = static_cast<float>(numAtProp(o, "densityFalloff",  opts.densityFalloff));
    opts.upBias         = static_cast<float>(numAtProp(o, "upBias",          opts.upBias));
    opts.tiltJitter     = static_cast<float>(numAtProp(o, "tiltJitter",      opts.tiltJitter));
    opts.rollJitter     = static_cast<float>(numAtProp(o, "rollJitter",      opts.rollJitter));
    opts.baseScale      = static_cast<float>(numAtProp(o, "baseScale",       opts.baseScale));
    opts.scaleJitter    = static_cast<float>(numAtProp(o, "scaleJitter",     opts.scaleJitter));
    opts.scaleByRadius  = static_cast<float>(numAtProp(o, "scaleByRadius",   opts.scaleByRadius));
    opts.dedupRadius    = static_cast<float>(numAtProp(o, "dedupRadius",     opts.dedupRadius));
    opts.seed           = static_cast<uint64_t>(numAtProp(o, "seed", static_cast<double>(opts.seed)));

    Value dw = ev::getProperty(o, "densityWeight");
    if (ev::isObject(dw)) {
        Value lenV = ev::getProperty(dw, "length");
        if (ev::isNumber(lenV)) {
            uint32_t n = static_cast<uint32_t>(ev::toDouble(lenV));
            opts.densityWeight.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                opts.densityWeight[i] = static_cast<float>(ev::toDouble(ev::getElement(dw, i)));
            }
        }
    }

    opts.avoid             = readAvoidField(o, "avoid");
    opts.obstacleClearance = static_cast<float>(numAtProp(o, "obstacleClearance", opts.obstacleClearance));
    opts.obstaclePushout   = static_cast<float>(numAtProp(o, "obstaclePushout",   opts.obstaclePushout));
    Value ko = ev::getProperty(o, "keepOut");
    if (ev::isObject(ko)) readSpheres(ko, opts.keepOut);
}

}  // namespace

void decorateMeshPrimitives(HostClass& cls) {
    // 1. box
    cls.setStatic("box", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float hw = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 0.5);
        float hh = static_cast<float>(hasArg(a, 1) ? ev::toDouble(a[1]) : 0.5);
        float hd = static_cast<float>(hasArg(a, 2) ? ev::toDouble(a[2]) : 0.5);
        return wrapMesh(bromesh::box(hw, hh, hd));
    }, 3));

    // 2. sphere
    cls.setStatic("sphere", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float r = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 0.5);
        int seg = hasArg(a, 1) ? static_cast<int>(ev::toDouble(a[1])) : 16;
        int rings = hasArg(a, 2) ? static_cast<int>(ev::toDouble(a[2])) : 12;
        return wrapMesh(bromesh::sphere(r, seg, rings));
    }, 3));

    // 3. cylinder
    cls.setStatic("cylinder", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float r = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 0.5);
        float hh = static_cast<float>(hasArg(a, 1) ? ev::toDouble(a[1]) : 0.5);
        int seg = hasArg(a, 2) ? static_cast<int>(ev::toDouble(a[2])) : 16;
        return wrapMesh(bromesh::cylinder(r, hh, seg));
    }, 3));

    // 4. capsule
    cls.setStatic("capsule", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float r = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 0.5);
        float hh = static_cast<float>(hasArg(a, 1) ? ev::toDouble(a[1]) : 0.5);
        int seg = hasArg(a, 2) ? static_cast<int>(ev::toDouble(a[2])) : 16;
        int rings = hasArg(a, 3) ? static_cast<int>(ev::toDouble(a[3])) : 8;
        return wrapMesh(bromesh::capsule(r, hh, seg, rings));
    }, 4));

    // 5. plane
    cls.setStatic("plane", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float hw = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 5.0);
        float hd = static_cast<float>(hasArg(a, 1) ? ev::toDouble(a[1]) : 5.0);
        int sx = hasArg(a, 2) ? static_cast<int>(ev::toDouble(a[2])) : 1;
        int sz = hasArg(a, 3) ? static_cast<int>(ev::toDouble(a[3])) : 1;
        return wrapMesh(bromesh::plane(hw, hd, sx, sz));
    }, 4));

    // 6. torus
    cls.setStatic("torus", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float major = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 1.0);
        float minor = static_cast<float>(hasArg(a, 1) ? ev::toDouble(a[1]) : 0.3);
        int majSeg = hasArg(a, 2) ? static_cast<int>(ev::toDouble(a[2])) : 24;
        int minSeg = hasArg(a, 3) ? static_cast<int>(ev::toDouble(a[3])) : 12;
        return wrapMesh(bromesh::torus(major, minor, majSeg, minSeg));
    }, 4));

    // 7. heightmapGrid
    cls.setStatic("heightmapGrid", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::throwTypeError("heightmapGrid requires (heights, gridW, gridH)");
        std::vector<float> heights;
        if (!readFloatVector(a[0], heights)) return ev::throwTypeError("heights must be Float32Array");
        int gw = static_cast<int>(ev::toDouble(a[1]));
        int gh = static_cast<int>(ev::toDouble(a[2]));
        float cs = static_cast<float>(hasArg(a, 3) ? ev::toDouble(a[3]) : 1.0);
        return wrapMesh(bromesh::heightmapGrid(heights.data(), gw, gh, cs));
    }, 4));

    // 8. geodesicSphere
    cls.setStatic("geodesicSphere", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float r = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 0.5);
        int nsub = hasArg(a, 1) ? static_cast<int>(ev::toDouble(a[1])) : 2;
        return wrapMesh(bromesh::geodesicSphere(r, nsub));
    }, 2));

    // 9. icosahedron
    cls.setStatic("icosahedron", ev::makeFunction([](Value, std::span<const Value>) -> Value {
        return wrapMesh(bromesh::icosahedron());
    }, 0));

    // 10. dodecahedron
    cls.setStatic("dodecahedron", ev::makeFunction([](Value, std::span<const Value>) -> Value {
        return wrapMesh(bromesh::dodecahedron());
    }, 0));

    // 11. octahedron
    cls.setStatic("octahedron", ev::makeFunction([](Value, std::span<const Value>) -> Value {
        return wrapMesh(bromesh::octahedron());
    }, 0));

    // 12. tetrahedron
    cls.setStatic("tetrahedron", ev::makeFunction([](Value, std::span<const Value>) -> Value {
        return wrapMesh(bromesh::tetrahedron());
    }, 0));

    // 13. cone
    cls.setStatic("cone", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float r = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 0.5);
        float h = static_cast<float>(hasArg(a, 1) ? ev::toDouble(a[1]) : 1.0);
        int slices = hasArg(a, 2) ? static_cast<int>(ev::toDouble(a[2])) : 16;
        int stacks = hasArg(a, 3) ? static_cast<int>(ev::toDouble(a[3])) : 4;
        bool capBase = hasArg(a, 4) ? ev::toBool(a[4]) : false;
        return wrapMesh(bromesh::cone(r, h, slices, stacks, capBase));
    }, 5));

    // 14. disc
    cls.setStatic("disc", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float r = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 0.5);
        int slices = hasArg(a, 1) ? static_cast<int>(ev::toDouble(a[1])) : 16;
        return wrapMesh(bromesh::disc(r, slices));
    }, 2));

    // 15. rock
    cls.setStatic("rock", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        float r = static_cast<float>(hasArg(a, 0) ? ev::toDouble(a[0]) : 0.5);
        int seed = hasArg(a, 1) ? static_cast<int>(ev::toDouble(a[1])) : 42;
        int nsub = hasArg(a, 2) ? static_cast<int>(ev::toDouble(a[2])) : 2;
        return wrapMesh(bromesh::rock(r, seed, nsub));
    }, 3));

    // 16. blob
    cls.setStatic("blob", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        double radius = 0.5;
        int seed = 42;
        int nsub = 2;
        bromath::Vec3 scale{1.0f, 1.0f, 1.0f};
        bromath::Vec3 center{0.0f, 0.0f, 0.0f};
        if (!a.empty() && ev::isObject(a[0])) {
            radius = numAtProp(a[0], "radius", radius);
            seed = intAtProp(a[0], "seed", seed);
            nsub = intAtProp(a[0], "nsub", nsub);
            Value sc = ev::getProperty(a[0], "scale");
            if (ev::isNumber(sc)) {
                float s = static_cast<float>(ev::toDouble(sc));
                scale = {s, s, s};
            } else if (ev::isObject(sc)) {
                scale = readBmVec3(sc);
            }
            Value cn = ev::getProperty(a[0], "center");
            if (ev::isObject(cn)) center = readBmVec3(cn);
        }
        return wrapMesh(bromesh::blob((float)radius, seed, nsub, scale.x, scale.y, scale.z, center.x, center.y, center.z));
    }, 1));

    // 17. sweep
    cls.setStatic("sweep", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("sweep requires (profile, path[, opts])");
        std::vector<bromath::Vec2> profile;
        std::vector<bromath::Vec3> path;
        if (!readVec2List(a[0], profile)) return ev::throwTypeError("profile must be Float32Array(2N) or [[x,y],...]");
        if (!readVec3List(a[1], path)) return ev::throwTypeError("path must be Float32Array(3N) or [[x,y,z],...]");
        bromesh::SweepOptions opts;
        if (a.size() > 2 && ev::isObject(a[2])) {
            opts.closeProfile = boolAtProp(a[2], "closeProfile", true);
            opts.capStart     = boolAtProp(a[2], "capStart",     true);
            opts.capEnd       = boolAtProp(a[2], "capEnd",       true);
            opts.miterJoints  = boolAtProp(a[2], "miterJoints",  true);
            readFloatVector(ev::getProperty(a[2], "profileScale"), opts.profileScale);
            readFloatVector(ev::getProperty(a[2], "twist"), opts.twist);
        }
        return wrapMesh(bromesh::sweep(profile, path, opts));
    }, 3));

    // 18. bezierSweep
    cls.setStatic("bezierSweep", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("bezierSweep requires (controlPoints, profile[, opts])");
        std::vector<bromath::Vec3> ctrl;
        std::vector<bromath::Vec2> profile;
        if (!readVec3List(a[0], ctrl) || ctrl.size() < 4) return ev::throwTypeError("controlPoints must have >= 4 Vec3s");
        if (!readVec2List(a[1], profile)) return ev::throwTypeError("profile must be Vec2 list");
        bromesh::BezierSweepOptions o;
        if (a.size() > 2 && ev::isObject(a[2])) {
            o.samples      = intAtProp(a[2], "samples", o.samples);
            o.capStart     = boolAtProp(a[2], "capStart", o.capStart);
            o.capEnd       = boolAtProp(a[2], "capEnd", o.capEnd);
            o.closeProfile = boolAtProp(a[2], "closeProfile", o.closeProfile);
            o.miterJoints  = boolAtProp(a[2], "miterJoints", o.miterJoints);
            readFloatVector(ev::getProperty(a[2], "profileScale"), o.profileScale);
            readFloatVector(ev::getProperty(a[2], "twist"), o.twist);
        }
        return wrapMesh(bromesh::bezierSweep(ctrl, profile, o));
    }, 3));

    // 19. tube
    cls.setStatic("tube", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("tube requires (path, radius[, sides, opts])");
        std::vector<bromath::Vec3> path;
        if (!readVec3List(a[0], path)) return ev::throwTypeError("path must be a Vec3 list");
        std::vector<float> radii;
        if (ev::isNumber(a[1])) {
            radii.push_back(static_cast<float>(ev::toDouble(a[1])));
        } else if (!readFloatVector(a[1], radii)) {
            return ev::throwTypeError("radius must be a number or float list");
        }
        bromesh::TubeOptions opts;
        if (a.size() > 2 && ev::isNumber(a[2])) opts.sides = static_cast<int>(ev::toDouble(a[2]));
        if (a.size() > 3 && ev::isObject(a[3])) {
            opts.capStart    = boolAtProp(a[3], "capStart",    opts.capStart);
            opts.capEnd      = boolAtProp(a[3], "capEnd",      opts.capEnd);
            opts.miterJoints = boolAtProp(a[3], "miterJoints", opts.miterJoints);
        }
        return wrapMesh(bromesh::tube(path, radii, opts));
    }, 4));

    // 20. leafCard
    cls.setStatic("leafCard", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("leafCard requires (shape[, opts])");
        bromesh::LeafShape shape = parseLeafShape(a[0]);
        bromesh::LeafCardOptions o;
        if (a.size() > 1 && ev::isObject(a[1])) {
            o.width            = static_cast<float>(numAtProp(a[1], "width",  o.width));
            o.length           = static_cast<float>(numAtProp(a[1], "length", o.length));
            o.bend             = static_cast<float>(numAtProp(a[1], "bend",   o.bend));
            o.curl             = static_cast<float>(numAtProp(a[1], "curl",   o.curl));
            o.stemOffset       = boolAtProp(a[1], "stemOffset", o.stemOffset);
            o.cup              = static_cast<float>(numAtProp(a[1], "cup",    o.cup));
            o.widthSegments    = intAtProp(a[1], "widthSegments",  o.widthSegments);
            o.lengthSegments   = intAtProp(a[1], "lengthSegments", o.lengthSegments);
            o.fullUV           = boolAtProp(a[1], "fullUV", o.fullUV);
            o.shapedSilhouette = boolAtProp(a[1], "shapedSilhouette", o.shapedSilhouette);
        }
        return wrapMesh(bromesh::leafCard(shape, o));
    }, 2));

    // 21. flower
    cls.setStatic("flower", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        bromesh::FlowerOptions o;
        if (!a.empty() && ev::isObject(a[0])) {
            o.petalCount        = intAtProp(a[0], "petalCount",   o.petalCount);
            Value ps = ev::getProperty(a[0], "petalShape");
            if (!ev::isUndefined(ps)) o.petalShape = parseLeafShape(ps);
            o.petalLength       = static_cast<float>(numAtProp(a[0], "petalLength",  o.petalLength));
            o.petalWidth        = static_cast<float>(numAtProp(a[0], "petalWidth",   o.petalWidth));
            o.petalCurl         = static_cast<float>(numAtProp(a[0], "petalCurl",    o.petalCurl));
            o.petalBend         = static_cast<float>(numAtProp(a[0], "petalBend",    o.petalBend));
            o.layers            = intAtProp(a[0], "layers",       o.layers);
            o.layerTwist        = static_cast<float>(numAtProp(a[0], "layerTwist",   o.layerTwist));
            o.centerRadius      = static_cast<float>(numAtProp(a[0], "centerRadius", o.centerRadius));
            o.centerHeight      = static_cast<float>(numAtProp(a[0], "centerHeight", o.centerHeight));
            o.outerTilt         = static_cast<float>(numAtProp(a[0], "outerTilt",    o.outerTilt));
            o.innerTilt         = static_cast<float>(numAtProp(a[0], "innerTilt",    o.innerTilt));
            o.layerScaleFalloff = static_cast<float>(numAtProp(a[0], "layerScaleFalloff", o.layerScaleFalloff));
            o.outerYLift        = static_cast<float>(numAtProp(a[0], "outerYLift",   o.outerYLift));
            o.innerYLift        = static_cast<float>(numAtProp(a[0], "innerYLift",   o.innerYLift));
            o.petalCup          = static_cast<float>(numAtProp(a[0], "petalCup",     o.petalCup));
            o.shapedPetals      = boolAtProp(a[0], "shapedPetals", o.shapedPetals);
            Value cc = ev::getProperty(a[0], "centerColor");
            if (ev::isObject(cc)) {
                for (int i = 0; i < 3; ++i) {
                    Value e = ev::getElement(cc, i);
                    if (!ev::isUndefined(e)) o.centerColor[i] = static_cast<float>(ev::toDouble(e));
                }
            }
        }
        return wrapMesh(bromesh::flower(o));
    }, 1));

    // 22. bladeStrip
    cls.setStatic("bladeStrip", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("bladeStrip requires (path[, opts])");
        std::vector<bromath::Vec3> path;
        if (!readVec3List(a[0], path)) return ev::throwTypeError("path must be a Vec3 list");
        bromesh::BladeStripOptions o;
        if (a.size() > 1 && ev::isObject(a[1])) {
            o.width       = static_cast<float>(numAtProp(a[1], "width",       o.width));
            o.thickness   = static_cast<float>(numAtProp(a[1], "thickness",   o.thickness));
            o.capStart    = boolAtProp(a[1], "capStart",    o.capStart);
            o.capEnd      = boolAtProp(a[1], "capEnd",      o.capEnd);
            o.miterJoints = boolAtProp(a[1], "miterJoints", o.miterJoints);
            readFloatVector(ev::getProperty(a[1], "profileScale"), o.profileScale);
            readFloatVector(ev::getProperty(a[1], "twist"), o.twist);
        }
        return wrapMesh(bromesh::bladeStrip(path, o));
    }, 2));

    // 23. bladePath
    cls.setStatic("bladePath", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        bromesh::BladePathOptions o;
        if (!a.empty() && ev::isObject(a[0])) {
            Value bv = ev::getProperty(a[0], "base");
            if (!ev::isUndefined(bv)) o.base = readBmVec3(bv);
            Value tv = ev::getProperty(a[0], "tipDir");
            if (!ev::isUndefined(tv)) o.tipDir = readBmVec3(tv);
            o.length   = static_cast<float>(numAtProp(a[0], "length",   o.length));
            o.bend     = static_cast<float>(numAtProp(a[0], "bend",     o.bend));
            o.lift     = static_cast<float>(numAtProp(a[0], "lift",     o.lift));
            o.segments = intAtProp(a[0], "segments", o.segments);
        }
        auto pts = bromesh::bladePath(o);
        return hostArrayOf(pts.size(), [&pts](size_t i) {
            std::vector<float> p = {pts[i].x, pts[i].y, pts[i].z};
            return hostArrayOf(3, [&p](size_t j) { return ev::fromDouble(p[j]); });
        });
    }, 1));

    // 24. spaceColonize
    cls.setStatic("spaceColonize", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 3) return ev::throwTypeError("spaceColonize requires (attractors, seedPoints, initialDirection[, opts])");
        std::vector<bromath::Vec3> attractors, seeds;
        if (!readVec3List(a[0], attractors)) return ev::throwTypeError("attractors must be Vec3 list");
        if (!readVec3List(a[1], seeds)) return ev::throwTypeError("seedPoints must be Vec3 list");
        bromath::Vec3 initDir = readBmVec3(a[2]);
        bromesh::SpaceColonizationOptions opts;
        if (a.size() > 3 && ev::isObject(a[3])) {
            opts.attractionRadius = static_cast<float>(numAtProp(a[3], "attractionRadius", opts.attractionRadius));
            opts.killRadius       = static_cast<float>(numAtProp(a[3], "killRadius",       opts.killRadius));
            opts.segmentLength    = static_cast<float>(numAtProp(a[3], "segmentLength",    opts.segmentLength));
            opts.maxIterations    = intAtProp(a[3], "maxIterations", opts.maxIterations);
            opts.tropismWeight    = static_cast<float>(numAtProp(a[3], "tropismWeight", opts.tropismWeight));
            Value tv = ev::getProperty(a[3], "tropism");
            if (!ev::isUndefined(tv)) opts.tropism = readBmVec3(tv);
            opts.obstacles        = readAvoidField(a[3], "obstacles");
        }
        auto segs = bromesh::spaceColonize(attractors, seeds, initDir, opts);
        return makeBranchSegments(segs);
    }, 4));

    // 25. thickenBranches
    cls.setStatic("thickenBranches", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("thickenBranches requires (segments[, leafRadius[, pipeExp]])");
        std::vector<bromesh::BranchSegment> segs;
        if (!readBranchSegments(a[0], segs)) return ev::throwTypeError("segments must be an array of branch segment objects");
        double leafR = hasArg(a, 1) ? ev::toDouble(a[1]) : 0.02;
        double pipeExp = hasArg(a, 2) ? ev::toDouble(a[2]) : 2.5;
        bromesh::thickenBranches(segs, static_cast<float>(leafR), static_cast<float>(pipeExp));
        return makeBranchSegments(segs);
    }, 3));

    // 26. meshBranches
    cls.setStatic("meshBranches", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("meshBranches requires (segments[, sides])");
        std::vector<bromesh::BranchSegment> segs;
        if (!readBranchSegments(a[0], segs)) return ev::throwTypeError("segments must be an array of branch segment objects");
        int sides = hasArg(a, 1) ? static_cast<int>(ev::toDouble(a[1])) : 8;
        if (sides < 3) sides = 8;
        return wrapMesh(bromesh::meshBranches(segs, sides));
    }, 2));

    // 27. placeLeavesOnBranches
    cls.setStatic("placeLeavesOnBranches", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.empty()) return ev::throwTypeError("placeLeavesOnBranches requires (segments[, opts])");
        std::vector<bromesh::BranchSegment> segs;
        if (!readBranchSegments(a[0], segs)) return ev::throwTypeError("segments must be an array of branch segment objects");
        bromesh::LeafPlacementOptions opts;
        if (a.size() > 1 && ev::isObject(a[1])) readLeafPlacementOptions(a[1], opts);
        auto p = bromesh::placeLeavesOnBranches(segs, opts);
        ObjectBuilder obj;
        obj.set("count", ev::fromDouble(static_cast<double>(p.count())));
        obj.set("transforms", makeFloat32Array(p.transforms.data(), p.transforms.size()));
        obj.set("branchRadius", makeFloat32Array(p.branchRadius.data(), p.branchRadius.size()));
        obj.set("branchDepth", makeInt32Array(p.branchDepth.data(), p.branchDepth.size()));
        return obj.get();
    }, 2));

    // 28. scatterLeaves
    cls.setStatic("scatterLeaves", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        if (a.size() < 2) return ev::throwTypeError("scatterLeaves requires (segments, leafMesh[, opts])");
        std::vector<bromesh::BranchSegment> segs;
        if (!readBranchSegments(a[0], segs)) return ev::throwTypeError("segments must be an array of branch segment objects");
        auto* lm = hostMeshDataOf(a[1]);
        if (!lm) return ev::throwTypeError("leaf must be a Mesh");
        bromesh::LeafPlacementOptions opts;
        if (a.size() > 2 && ev::isObject(a[2])) readLeafPlacementOptions(a[2], opts);
        return wrapMesh(bromesh::scatterLeaves(segs, *lm, opts));
    }, 3));

    // 29. tree
    cls.setStatic("tree", ev::makeFunction([](Value, std::span<const Value> a) -> Value {
        bromesh::TreeOptions o;
        if (!a.empty() && ev::isObject(a[0])) {
            Value bv = ev::getProperty(a[0], "base");
            if (!ev::isUndefined(bv)) o.base = readBmVec3(bv);
            Value cv = ev::getProperty(a[0], "canopyCenter");
            if (!ev::isUndefined(cv)) o.canopyCenter = readBmVec3(cv);
            o.canopyRadius   = static_cast<float>(numAtProp(a[0], "canopyRadius",   o.canopyRadius));
            o.attractorCount = intAtProp(a[0], "attractorCount", o.attractorCount);
            o.sides          = intAtProp(a[0], "sides",          o.sides);
            o.leafRadius     = static_cast<float>(numAtProp(a[0], "leafRadius",     o.leafRadius));
            o.pipeExp        = static_cast<float>(numAtProp(a[0], "pipeExp",        o.pipeExp));
            o.seed           = intAtProp(a[0], "seed",           o.seed);
            Value colv = ev::getProperty(a[0], "colonize");
            if (ev::isObject(colv)) {
                o.colonize.attractionRadius = static_cast<float>(numAtProp(colv, "attractionRadius", o.colonize.attractionRadius));
                o.colonize.killRadius       = static_cast<float>(numAtProp(colv, "killRadius",       o.colonize.killRadius));
                o.colonize.segmentLength    = static_cast<float>(numAtProp(colv, "segmentLength",    o.colonize.segmentLength));
                o.colonize.maxIterations    = intAtProp(colv, "maxIterations", o.colonize.maxIterations);
                o.colonize.tropismWeight    = static_cast<float>(numAtProp(colv, "tropismWeight", o.colonize.tropismWeight));
                Value tv = ev::getProperty(colv, "tropism");
                if (!ev::isUndefined(tv)) o.colonize.tropism = readBmVec3(tv);
                o.colonize.obstacles        = readAvoidField(colv, "obstacles");
            }
        }
        auto r = bromesh::tree(o);
        ObjectBuilder out;
        out.set("segments", makeBranchSegments(r.segments));
        out.set("branches", wrapMesh(std::move(r.branches)));
        return out.get();
    }, 1));
}

}  // namespace bro::bronze_host

#endif  // BRO_WITH_3D
