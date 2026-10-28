#include "scene/world/plant_canopy.h"

#include "scene/world/plant_placement.h"

#include "api/noise_glsl.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace bro::scene {

namespace {

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    std::string s = buf;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

}  // namespace

bool plantCanopyModule(const PlacementRules& rules, const float* species, int speciesCount, int speciesFloats,
                       std::string& out, std::string& err) {
    // Each field once, a function of its own.
    std::string fields;
    std::map<const Field*, std::string> names;
    const auto call = [&](const FieldRef& f, const char* none, std::string& expr) {
        if (!f) {
            expr = none;
            return true;
        }
        auto it = names.find(f.get());
        if (it == names.end()) {
            const std::string name = "plantsCanopyField" + std::to_string(names.size());
            brokit::noise::GlslOptions o;
            o.name = name;
            o.dims3 = true;
            std::string glsl;
            if (!brokit::noise::treeToGlsl(f->encoded(), o, glsl, err)) return false;
            fields += glsl;
            it = names.emplace(f.get(), name).first;
        }
        expr = it->second + "(p, " + std::to_string(f->seed()) + ")";
        return true;
    };
    // A term: its field times the land's own value (its grid's channel 0's
    // z or w at the point, scene_land_layer.glsl's landGrid).
    const auto term = [&](const PlacementTerm& t, const char* none, std::string& expr) {
        if (t.none()) {
            expr = none;
            return true;
        }
        if (!call(t.field, "1.0", expr)) return false;
        if (t.own >= 0)
            expr = "(" + expr + ") * (" + num(t.offset) + " + " + num(t.scale) + " * landGrid(0, p)." +
                   (t.own == 0 ? "z" : "w") + ")";
        return true;
    };

    std::string body;
    for (const PlacementLayer& layer : rules.layers) {
        if (!(layer.spacing > 0.0f) || layer.species.empty()) continue;
        std::string chance, scale;
        if (!term(layer.chance, "1.0", chance) || !term(layer.scale, "1.0", scale)) return false;
        body += "    {\n";
        body += "        float chance = clamp(" + chance + ", 0.0, 1.0);\n";
        body += "        float s = max(" + scale + ", 0.0);\n";
        body += "        float s2 = s * s * " + num(1.0 + layer.scaleVariety * layer.scaleVariety / 3.0) + ";\n";
        body += "        float sum = 0.0, area = 0.0, top = 0.0, side = 0.0, base = 0.0;\n";
        body += "        vec3 alb = vec3(0.0);\n";
        for (const PlacementLayer::Choice& c : layer.species) {
            if (c.species < 0 || c.species >= speciesCount) continue;
            const float* rec = species + static_cast<size_t>(c.species) * speciesFloats;
            const double height = rec[0], radius = rec[2];
            const double crown = 3.14159265358979 * radius * radius;
            // (Its crown seen from the side: a round crown's ellipse, a
            // cone's triangle, its depth under the top by its base.)
            const double depth = height * (1.0 - std::clamp(double(rec[1]), 0.0, 1.0));
            const double shape = std::clamp(double(rec[6]), 0.0, 1.0);
            const double flank = (1.5707963267949 + (1.0 - 1.5707963267949) * shape) * radius * depth;
            std::string w;
            if (!term(c.weight, "1.0", w)) return false;
            body += "        {\n";
            body += "            float w = max(" + w + ", 0.0);\n";
            body += "            sum += w;\n";
            body += "            area += w * " + num(crown) + ";\n";
            body += "            top += w * " + num(crown * height) + ";\n";
            body += "            side += w * " + num(flank) + ";\n";
            body += "            base += w * " + num(crown * (height - depth)) + ";\n";
            body += "            alb += w * " + num(crown) + " * vec3(" + num(rec[8]) + ", " + num(rec[9]) + ", " +
                    num(rec[10]) + ");\n";
            body += "        }\n";
        }
        // (Per square metre: the layer's candidates, those that grow, their
        // crowns at the mean square size; the top at the mean size.)
        body += "        float k = sum > 0.0 ? chance * s2 / (" + num(double(layer.spacing) * layer.spacing) +
                " * sum) : 0.0;\n";
        body += "        tau += k * area;\n";
        body += "        sides += k * side;\n";
        body += "        tops += k * top * s;\n";
        body += "        bases += k * base * s;\n";
        body += "        albs += k * alb;\n";
        body += "    }\n";
    }

    out = "// The plants' canopy (world/plant_canopy.h), written from their rules.\n";
    out += fields;
    // (At a point of the plane, and of a round body's datum.)
    for (const char* point : {"vec2", "vec3"}) {
        out += std::string("vec4 plantsCanopy(") + point + " p, out vec3 albedo) {\n";
        out += "    float tau = 0.0, tops = 0.0, sides = 0.0, bases = 0.0;\n";
        out += "    vec3 albs = vec3(0.0);\n";
        out += body;
        out += "    albedo = tau > 0.0 ? albs / tau : vec3(0.0);\n";
        out += "    return vec4(1.0 - exp(-tau), tau > 0.0 ? tops / tau : 0.0, sides, tau > 0.0 ? bases / tau : 0.0);\n";
        out += "}\n";
    }
    return true;
}

}  // namespace bro::scene
