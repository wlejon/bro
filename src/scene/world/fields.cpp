#include "scene/world/fields.h"

#include <FastNoise/FastNoise.h>

namespace bro::scene {

std::shared_ptr<const Field> Field::make(const std::string& encoded, int seed) {
    FastNoise::SmartNode<> node = FastNoise::NewFromEncodedNodeTree(encoded.c_str());
    if (!node) return nullptr;
    std::shared_ptr<Field> f(new Field());
    f->encoded_ = encoded;
    f->seed_ = seed;
    f->node_ = std::make_unique<FastNoise::SmartNode<>>(std::move(node));
    return f;
}

Field::~Field() = default;

float Field::at(float x, float z) const { return (*node_)->GenSingle2D(x, z, seed_); }

void Field::sample(const float* xs, const float* zs, int count, float* out) const {
    if (count > 0) (*node_)->GenPositionArray2D(out, count, xs, zs, 0.0f, 0.0f, seed_);
}

}  // namespace bro::scene
