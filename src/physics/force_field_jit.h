#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "codegen-brass/js_gpu_autodiff.h"
#include "codegen-brass/js_gpu_kernel.h"
#include "physics/physics_world.h"

namespace bro::physics {

struct ForceFieldSample {
    float energy = 0.0f;
    float fx = 0.0f;
    float fy = 0.0f;
    float fz = 0.0f;
};

struct ForceFieldCompileOptions {
    bool isPotential = true;      // True: F(x,y,z,t) = -grad U(x,y,z,t) via symbolic autodiff
    float scale = 1.0f;           // Scalar multiplier applied to resulting force
    float damping = 0.0f;         // Linear velocity damping coefficient (F_damp = -damping * v)
    float maxForce = 0.0f;        // > 0 clamps force magnitude ||F|| <= maxForce
    bool enableParallel = true;   // Dispatch batch particle/body updates via brass::codegen::parallel_for
    std::string kernelName;
};

struct EnergyConservationReport {
    double initialEnergy = 0.0;
    double finalEnergy = 0.0;
    double minEnergy = 0.0;
    double maxEnergy = 0.0;
    double maxRelativeDrift = 0.0;
    uint32_t steps = 0;
    bool conserved = false;
};

class CompiledForceField {
public:
    CompiledForceField() = default;

    // Compile a scalar potential U(x, y, z, t) into an analytical conservative force
    // field F(x, y, z, t) = -grad U(x, y, z, t) using Bronze/Brass symbolic autodiff.
    static CompiledForceField compilePotential(
        const std::string& jsSource,
        const ForceFieldCompileOptions& opts = {});

    // Compile a force field from a JS function source (defaults to potential mode unless
    // opts.isPotential == false).
    static CompiledForceField compileForceField(
        const std::string& jsSource,
        const ForceFieldCompileOptions& opts = {});

    // Compile a 3-component vector force field (Fx(x,y,z,t), Fy(x,y,z,t), Fz(x,y,z,t))
    // from three scalar JS functions.
    static CompiledForceField compileVectorForce(
        const std::string& fxSource,
        const std::string& fySource,
        const std::string& fzSource,
        const ForceFieldCompileOptions& opts = {});

    bool ok() const { return ok_; }
    bool isPotential() const { return isPotential_; }
    const std::string& errorMessage() const { return errorMessage_; }
    const std::vector<std::string>& diagnostics() const { return diagnostics_; }
    const ForceFieldCompileOptions& options() const { return opts_; }
    const bronze::GpuAutodiffResult& autodiffResult() const { return potentialAutodiff_; }

    // Evaluate scalar potential energy U(x, y, z, t) at a single point.
    float evalPotential(float x, float y, float z, float t = 0.0f) const;

    // Evaluate both potential energy U and force vector (fx, fy, fz) at (x, y, z, t).
    ForceFieldSample evalAt(float x, float y, float z, float t = 0.0f) const;

    // Evaluate force vector (fx, fy, fz) across N 3D positions in parallel.
    // positionsXYZ and outForcesXYZ are packed [x0, y0, z0, x1, y1, z1, ...] of length 3 * count.
    void evalForceBatch(
        const float* positionsXYZ,
        size_t count,
        float t,
        float* outForcesXYZ) const;

    // Integrate N particles in-place using symplectic Euler:
    //   v_i += ((F(x_i, t) - damping * v_i) / m_i) * dt
    //   x_i += v_i * dt
    // Executed in native parallel chunks via brass::codegen::parallel_for with zero JS calls.
    size_t applyToParticles(
        float* positionsXYZ,
        float* velocitiesXYZ,
        size_t particleCount,
        float dt,
        float defaultMass = 1.0f,
        float t = 0.0f,
        const float* perParticleMasses = nullptr) const;

    // Apply the compiled force field to all active dynamic rigid bodies (or a specified
    // subset) in a Jolt PhysicsWorld. Returns the number of dynamic bodies updated.
    size_t applyToPhysicsWorld(
        PhysicsWorld& world,
        float t = 0.0f,
        const std::vector<JPH::BodyID>* specificBodies = nullptr) const;

    // Symplectic energy conservation verification over `steps` timesteps for a test particle.
    EnergyConservationReport verifyConservation(
        float x0,
        float y0,
        float z0,
        float vx0,
        float vy0,
        float vz0,
        float dt = 0.001f,
        uint32_t steps = 1000,
        float mass = 1.0f,
        float tolerance = 1e-3f) const;

private:
    void clampForce(float& fx, float& fy, float& fz) const noexcept;

    bool ok_ = false;
    bool isPotential_ = true;
    bool isVector3_ = false;
    ForceFieldCompileOptions opts_;
    bronze::GpuAutodiffResult potentialAutodiff_;
    bronze::GpuCompileResult forceComponents_[3];
    std::vector<std::string> diagnostics_;
    std::string errorMessage_;
};

}  // namespace bro::physics
