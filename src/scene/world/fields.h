#pragma once
// A field (docs/scene-world-architecture.md §10, §11): a value over the
// ground, a FastNoise2 node tree with a seed. A tree comes as data, the
// encoded string FastNoise2's own tools and bro's FastNoise binding
// (node.encode()) write, so a world's fields are a table it can store,
// share between threads and hand from script to the engine; the engine
// decodes its own copy, which no script can change under a worker.
//
// Positions are metres on the ground, x and z (FastNoise2's x and y);
// a tree's own scale (its nodes' "Feature Scale") says how fast it varies.
// A tree is read-only once made, so any thread may sample it.

#include <memory>
#include <string>

namespace FastNoise {
class Generator;
template <typename T>
class SmartNode;
}  // namespace FastNoise

namespace bro::scene {

class Field {
public:
    /// The tree `encoded` with `seed`; null when it does not decode.
    static std::shared_ptr<const Field> make(const std::string& encoded, int seed);
    ~Field();

    float at(float x, float z) const;
    /// `count` points' values: (xs[i], zs[i]) into out[i].
    void sample(const float* xs, const float* zs, int count, float* out) const;

    const std::string& encoded() const { return encoded_; }
    int seed() const { return seed_; }

private:
    Field() = default;
    std::string encoded_;
    int seed_ = 0;
    std::unique_ptr<FastNoise::SmartNode<FastNoise::Generator>> node_;
};

using FieldRef = std::shared_ptr<const Field>;

}  // namespace bro::scene
