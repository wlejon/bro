#include "physics/force_field_jit.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace bro::physics;

namespace {

void testPotentialGradAccuracy() {
    const std::string src = R"(
// @kernel
function harmonicGravPotential(x, y, z) {
    let k = 2.5;
    let gm = 10.0;
    let eps2 = 0.25;
    let r2 = x * x + y * y + z * z;
    return 0.5 * k * r2 - gm / Math.sqrt(r2 + eps2);
}
)";
    CompiledForceField field = CompiledForceField::compilePotential(src);
    assert(field.ok());
    assert(field.isPotential());
    assert(field.autodiffResult().provenSafe);
    assert(field.autodiffResult().cpuGpuProvenEquivalent);

    const float k = 2.5f;
    const float gm = 10.0f;
    const float eps2 = 0.25f;
    const float coords[][3] = {
        {1.0f, 2.0f, -1.5f},
        {-2.5f, 0.75f, 3.25f},
        {0.0f, 0.0f, 0.0f},
        {4.0f, -3.0f, 1.25f},
        {-0.5f, -1.25f, -2.0f},
    };

    for (const auto& pt : coords) {
        const float x = pt[0], y = pt[1], z = pt[2];
        const float r2 = x * x + y * y + z * z;
        const float rEps = std::sqrt(r2 + eps2);
        const float rEps3 = rEps * rEps * rEps;

        const float expectedU = 0.5f * k * r2 - gm / rEps;
        const float expectedFx = -(k * x + gm * x / rEps3);
        const float expectedFy = -(k * y + gm * y / rEps3);
        const float expectedFz = -(k * z + gm * z / rEps3);

        const ForceFieldSample s = field.evalAt(x, y, z);
        const float denomU = std::max(std::fabs(expectedU), 1.0f);
        const float denomF = std::max(std::sqrt(expectedFx * expectedFx +
                                                expectedFy * expectedFy +
                                                expectedFz * expectedFz), 1.0f);

        assert(std::fabs(s.energy - expectedU) / denomU < 1e-5f);
        assert(std::fabs(s.fx - expectedFx) / denomF < 1e-5f);
        assert(std::fabs(s.fy - expectedFy) / denomF < 1e-5f);
        assert(std::fabs(s.fz - expectedFz) / denomF < 1e-5f);
    }
    std::printf("[PASS] Test 1: Potential Field -grad U Accuracy (< 1e-5 relative error)\n");
}

void testSymplecticEnergyConservation() {
    const std::string src = R"(
// @kernel
function isotropicHarmonic(x, y, z) {
    let k = 1.5;
    return 0.5 * k * (x * x + y * y + z * z);
}
)";
    CompiledForceField harmonic = CompiledForceField::compileForceField(src);
    assert(harmonic.ok());

    const EnergyConservationReport rep = harmonic.verifyConservation(
        2.0f, -1.0f, 0.5f,
        0.25f, 1.5f, -0.75f,
        0.001f, 1000, 1.0f, 1e-3f);

    assert(rep.steps == 1000);
    assert(rep.initialEnergy > 0.0);
    assert(rep.maxRelativeDrift < 1e-3);
    assert(rep.conserved);
    std::printf("[PASS] Test 2: Symplectic Energy Conservation (maxRelativeDrift = %.6g < 1e-3)\n",
                rep.maxRelativeDrift);
}

void testParallelBatchThroughputAndJoltIntegration() {
    const std::string src = R"(
// @kernel
function radialWell(x, y, z, t) {
    let r2 = x * x + y * y + z * z;
    return 0.5 * 2.0 * r2 + 0.1 * Math.sin(x + t);
}
)";
    CompiledForceField field = CompiledForceField::compileForceField(src);
    assert(field.ok());

    constexpr size_t kParticleCount = 10000;
    std::vector<float> positions(kParticleCount * 3);
    std::vector<float> velocities(kParticleCount * 3, 0.0f);
    for (size_t i = 0; i < kParticleCount; ++i) {
        positions[i * 3 + 0] = static_cast<float>(i % 100) * 0.1f - 5.0f;
        positions[i * 3 + 1] = static_cast<float>(i / 100) * 0.1f - 5.0f;
        positions[i * 3 + 2] = 1.0f;
    }

    // Warmup
    field.applyToParticles(positions.data(), velocities.data(), kParticleCount, 0.001f, 1.0f, 0.0f);

    const auto t0 = std::chrono::steady_clock::now();
    const size_t updated = field.applyToParticles(
        positions.data(), velocities.data(), kParticleCount, 0.001f, 1.0f, 0.25f);
    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    assert(updated == kParticleCount);
    assert(elapsedMs < 5.0);
    assert(std::isfinite(positions[0]) && std::isfinite(velocities[0]));

    PhysicsWorld world;
    world.init(1024, 1024);
    world.setGravity(0.0f, 0.0f, 0.0f);

    BodyOptions b1Opts;
    b1Opts.shape = BodyOptions::ShapeSphere;
    b1Opts.position = JPH::RVec3(3.0f, 0.0f, 0.0f);
    b1Opts.radius = 0.5f;
    b1Opts.mass = 1.0f;
    b1Opts.linearDamping = 0.0f;
    const JPH::BodyID b1 = world.createBody(b1Opts);

    BodyOptions b2Opts = b1Opts;
    b2Opts.position = JPH::RVec3(0.0f, 4.0f, 0.0f);
    const JPH::BodyID b2 = world.createBody(b2Opts);

    const size_t bodiesUpdated = field.applyToPhysicsWorld(world, 0.0f);
    assert(bodiesUpdated == 2);
    world.setTimeStep(1.0f / 60.0f);
    world.stepInline();

    assert(world.getPosition(b1).GetX() < 3.0f);
    assert(world.getPosition(b2).GetY() < 4.0f);
    world.shutdown();

    std::printf("[PASS] Test 3: Parallel Batch 10,000 Particles (%.3f ms < 5.0 ms) & Jolt PhysicsWorld\n",
                elapsedMs);
}

}  // namespace

int main() {
    testPotentialGradAccuracy();
    testSymplecticEnergyConservation();
    testParallelBatchThroughputAndJoltIntegration();
    std::printf("All force_field_jit tests passed!\n");
    return 0;
}
