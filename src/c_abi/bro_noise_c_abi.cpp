// =============================================================================
// bro_noise_c_abi.cpp — C++ forwarding implementations for bro.noise
// Generated automatically by brosurface (gen/emit_c_abi.mjs).
// =============================================================================

#include "bro/c_abi/bro_noise_c_abi.h"
#include <FastNoise/FastNoise.h>
#include <FastNoise/Metadata.h>
#include <cstring>

extern "C" {

struct FastNoiseHandle {
    FastNoise::SmartNode<> node;
};

void* bro_FastNoise_create_from_encoded(const char* encodedNodeTree) {
    if (!encodedNodeTree) return nullptr;
    auto node = FastNoise::NewFromEncodedNodeTree(encodedNodeTree);
    if (!node) return nullptr;
    return new FastNoiseHandle{std::move(node)};
}

void* bro_FastNoise_create(const char* typeName) {
    if (!typeName) return nullptr;
    for (const auto* meta : FastNoise::Metadata::GetAll()) {
        if (meta && strcmp(meta->name, typeName) == 0) {
            auto node = meta->CreateNode();
            if (!node) return nullptr;
            return new FastNoiseHandle{std::move(node)};
        }
    }
    return nullptr;
}

void* bro_FastNoise_Simplex(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::Simplex>()};
}

void* bro_FastNoise_SuperSimplex(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::SuperSimplex>()};
}

void* bro_FastNoise_Perlin(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::Perlin>()};
}

void* bro_FastNoise_Value(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::Value>()};
}

void* bro_FastNoise_CellularValue(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::CellularValue>()};
}

void* bro_FastNoise_CellularDistance(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::CellularDistance>()};
}

void* bro_FastNoise_CellularLookup(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::CellularLookup>()};
}

void* bro_FastNoise_FractalFBm(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::FractalFBm>()};
}

void* bro_FastNoise_FractalRidged(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::FractalRidged>()};
}

void* bro_FastNoise_DomainWarpGradient(void) {
    return new FastNoiseHandle{FastNoise::New<FastNoise::DomainWarpGradient>()};
}

void bro_FastNoise_destroy(void* self) {
    if (self) {
        delete static_cast<FastNoiseHandle*>(self);
    }
}

double bro_FastNoise_genSingle2D(void* self, double x, double y, int32_t seed) {
    if (!self) return 0.0;
    auto* h = static_cast<FastNoiseHandle*>(self);
    if (!h->node) return 0.0;
    return static_cast<double>(h->node->GenSingle2D(static_cast<float>(x), static_cast<float>(y), seed));
}

double bro_FastNoise_genSingle3D(void* self, double x, double y, double z, int32_t seed) {
    if (!self) return 0.0;
    auto* h = static_cast<FastNoiseHandle*>(self);
    if (!h->node) return 0.0;
    return static_cast<double>(h->node->GenSingle3D(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), seed));
}

static bool fnMemberMatches(const char* query, const FastNoise::Metadata::Member& m) {
    if (m.dimensionIdx < 0) {
        return strcmp(query, m.name) == 0;
    }
    size_t baseLen = strlen(m.name);
    if (strncmp(query, m.name, baseLen) != 0) return false;
    if (query[baseLen] != ' ') return false;
    char dim = query[baseLen + 1];
    if (query[baseLen + 2] != '\0') return false;
    int queryIdx = (dim == 'X') ? 0 : (dim == 'Y') ? 1 : (dim == 'Z') ? 2 : (dim == 'W') ? 3 : -1;
    return queryIdx == m.dimensionIdx;
}

void bro_FastNoise_set(void* self, const char* name, double val) {
    if (!self || !name) return;
    auto* h = static_cast<FastNoiseHandle*>(self);
    if (!h->node) return;
    const auto& meta = h->node->GetMetadata();
    for (const auto& mv : meta.memberVariables) {
        if (!fnMemberMatches(name, mv)) continue;
        if (mv.type == FastNoise::Metadata::MemberVariable::EFloat) {
            mv.setFunc(h->node.get(), FastNoise::Metadata::MemberVariable::ValueUnion(static_cast<float>(val)));
            return;
        }
        if (mv.type == FastNoise::Metadata::MemberVariable::EInt || mv.type == FastNoise::Metadata::MemberVariable::EEnum) {
            mv.setFunc(h->node.get(), FastNoise::Metadata::MemberVariable::ValueUnion(static_cast<int>(val)));
            return;
        }
    }
    for (const auto& mh : meta.memberHybrids) {
        if (!fnMemberMatches(name, mh)) continue;
        mh.setValueFunc(h->node.get(), static_cast<float>(val));
        return;
    }
}

void bro_FastNoise_set_node(void* self, const char* name, void* node) {
    if (!self || !name || !node) return;
    auto* h = static_cast<FastNoiseHandle*>(self);
    auto* srcH = static_cast<FastNoiseHandle*>(node);
    if (!h->node || !srcH->node) return;
    const auto& meta = h->node->GetMetadata();
    for (const auto& mn : meta.memberNodeLookups) {
        if (!fnMemberMatches(name, mn)) continue;
        mn.setFunc(h->node.get(), srcH->node);
        return;
    }
    for (const auto& mh : meta.memberHybrids) {
        if (!fnMemberMatches(name, mh)) continue;
        mh.setNodeFunc(h->node.get(), srcH->node);
        return;
    }
}

} // extern "C"
