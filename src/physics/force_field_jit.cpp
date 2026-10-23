#include "physics/force_field_jit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <Jolt/Physics/Body/BodyInterface.h>
#include <brass/codegen/kernel_jit.hpp>

namespace bro::physics {

namespace {

std::string trimWs(std::string s) {
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string normalizeForceJsSource(const std::string& raw, const std::string& fallbackName) {
    std::string src = trimWs(raw);
    if (src.empty()) return src;

    const std::string fnName = fallbackName.empty() ? "__bro_force_field" : fallbackName;
    const bool startsWithComment = (src.rfind("//", 0) == 0 || src.rfind("/*", 0) == 0);
    const bool hasFunctionKw = (src.find("function ") != std::string::npos ||
                                src.find("function(") != std::string::npos);
    if (!startsWithComment && !hasFunctionKw) {
        const size_t arrowPos = src.find("=>");
        if (arrowPos != std::string::npos) {
            std::string params = trimWs(src.substr(0, arrowPos));
            std::string body = trimWs(src.substr(arrowPos + 2));
            if (params.empty() || params.front() != '(') params = "(" + params + ")";
            if (!body.empty() && body.front() == '{') {
                src = "function " + fnName + params + " " + body;
            } else {
                if (!body.empty() && body.back() == ';') body.pop_back();
                src = "function " + fnName + params + " { return (" + body + "); }";
            }
        }
    } else if (src.rfind("function(", 0) == 0) {
        src = "function " + fnName + src.substr(8);
    } else if (src.rfind("function (", 0) == 0) {
        src = "function " + fnName + src.substr(9);
    }

    if (src.find("@kernel") == std::string::npos &&
        src.find("@shader") == std::string::npos) {
        src = "// @kernel\n" + src;
    }
    return src;
}

}  // namespace

CompiledForceField CompiledForceField::compilePotential(
    const std::string& jsSource,
    const ForceFieldCompileOptions& opts) {
    ForceFieldCompileOptions potOpts = opts;
    potOpts.isPotential = true;
    return compileForceField(jsSource, potOpts);
}

CompiledForceField CompiledForceField::compileForceField(
    const std::string& jsSource,
    const ForceFieldCompileOptions& opts) {
    CompiledForceField out;
    out.opts_ = opts;
    out.isPotential_ = opts.isPotential;
    out.isVector3_ = false;

    const std::string normalized = normalizeForceJsSource(jsSource, opts.kernelName);
    if (normalized.empty()) {
        out.errorMessage_ = "Empty force field JS source";
        out.diagnostics_.push_back(out.errorMessage_);
        return out;
    }

    bronze::GpuAutodiffOptions adOpts;
    adOpts.kernelName = opts.kernelName;
    adOpts.stage = brass::target::SpirvStage::Compute;
    adOpts.runOptimizations = true;
    adOpts.verifyEquivalence = true;
    adOpts.verifySafety = true;
    adOpts.compileCpu = true;
    adOpts.verifySingleSourceParity = true;
    adOpts.allowUnannotated = true;
    adOpts.forceHeightfieldNormal = false;

    out.potentialAutodiff_ = bronze::compileJsGpuAutodiffSource(normalized, "force_field.js", adOpts);
    out.diagnostics_ = out.potentialAutodiff_.diagnostics;
    out.errorMessage_ = out.potentialAutodiff_.errorMessage;
    out.ok_ = out.potentialAutodiff_.ok;
    return out;
}

CompiledForceField CompiledForceField::compileVectorForce(
    const std::string& fxSource,
    const std::string& fySource,
    const std::string& fzSource,
    const ForceFieldCompileOptions& opts) {
    CompiledForceField out;
    out.opts_ = opts;
    out.isPotential_ = false;
    out.isVector3_ = true;

    const std::string sources[3] = {
        normalizeForceJsSource(fxSource, "__bro_fx"),
        normalizeForceJsSource(fySource, "__bro_fy"),
        normalizeForceJsSource(fzSource, "__bro_fz")
    };
    const char* names[3] = {"fx.js", "fy.js", "fz.js"};

    for (int k = 0; k < 3; ++k) {
        bronze::GpuCompileOptions kOpts;
        kOpts.stage = brass::target::SpirvStage::Compute;
        kOpts.runOptimizations = true;
        kOpts.compileCpu = true;
        out.forceComponents_[k] = bronze::compileJsGpuKernelSource(sources[k], names[k], kOpts);
        for (const auto& d : out.forceComponents_[k].diagnostics) {
            out.diagnostics_.push_back(d);
        }
        if (!out.forceComponents_[k].ok) {
            out.errorMessage_ = out.forceComponents_[k].errorMessage;
            out.ok_ = false;
            return out;
        }
    }
    out.ok_ = true;
    return out;
}

void CompiledForceField::clampForce(float& fx, float& fy, float& fz) const noexcept {
    if (opts_.maxForce > 0.0f) {
        const float magSq = fx * fx + fy * fy + fz * fz;
        const float maxSq = opts_.maxForce * opts_.maxForce;
        if (magSq > maxSq && magSq > 1e-20f) {
            const float inv = opts_.maxForce / std::sqrt(magSq);
            fx *= inv;
            fy *= inv;
            fz *= inv;
        }
    }
}

float CompiledForceField::evalPotential(float x, float y, float z, float t) const {
    if (!ok_) return 0.0f;
    if (isVector3_) return 0.0f;
    return potentialAutodiff_.evalGradCpu(x, y, z, t).value * opts_.scale;
}

ForceFieldSample CompiledForceField::evalAt(float x, float y, float z, float t) const {
    ForceFieldSample s;
    if (!ok_) return s;

    if (isVector3_) {
        const std::vector<float> args = {x, y, z, t};
        s.energy = 0.0f;
        s.fx = forceComponents_[0].evalCpu(args) * opts_.scale;
        s.fy = forceComponents_[1].evalCpu(args) * opts_.scale;
        s.fz = forceComponents_[2].evalCpu(args) * opts_.scale;
        clampForce(s.fx, s.fy, s.fz);
        return s;
    }

    const bronze::GpuAutodiffSample g = potentialAutodiff_.evalGradCpu(x, y, z, t);
    if (isPotential_) {
        s.energy = g.value * opts_.scale;
        // Conservative force F = -nabla U(x, y, z, t)
        s.fx = -g.dx * opts_.scale;
        if (potentialAutodiff_.scalarParamCount == 2 && potentialAutodiff_.isHeightfield2D) {
            s.fy = 0.0f;
            s.fz = -g.dz * opts_.scale;
        } else {
            s.fy = -g.dy * opts_.scale;
            s.fz = -g.dz * opts_.scale;
        }
    } else {
        s.energy = g.value * opts_.scale;
        s.fx = g.dx * opts_.scale;
        s.fy = g.dy * opts_.scale;
        s.fz = g.dz * opts_.scale;
    }
    clampForce(s.fx, s.fy, s.fz);
    return s;
}

void CompiledForceField::evalForceBatch(
    const float* positionsXYZ,
    size_t count,
    float t,
    float* outForcesXYZ) const {
    if (!ok_ || !positionsXYZ || !outForcesXYZ || count == 0) return;

    auto evalRange = [&](int64_t begin, int64_t end) {
        for (int64_t i = begin; i < end; ++i) {
            const float* p = positionsXYZ + static_cast<size_t>(i) * 3;
            const ForceFieldSample s = evalAt(p[0], p[1], p[2], t);
            float* dst = outForcesXYZ + static_cast<size_t>(i) * 3;
            dst[0] = s.fx;
            dst[1] = s.fy;
            dst[2] = s.fz;
        }
    };

    if (opts_.enableParallel && count >= 64) {
        brass::codegen::parallel_for(0, static_cast<int64_t>(count), evalRange, 64);
    } else {
        evalRange(0, static_cast<int64_t>(count));
    }
}

size_t CompiledForceField::applyToParticles(
    float* positionsXYZ,
    float* velocitiesXYZ,
    size_t particleCount,
    float dt,
    float defaultMass,
    float t,
    const float* perParticleMasses) const {
    if (!ok_ || !positionsXYZ || !velocitiesXYZ || particleCount == 0 || dt == 0.0f) {
        return 0;
    }

    const float safeDefaultMass = (defaultMass > 1e-12f) ? defaultMass : 1.0f;
    const float damp = opts_.damping;

    auto stepRange = [&](int64_t begin, int64_t end) {
        for (int64_t idx = begin; idx < end; ++idx) {
            const size_t i = static_cast<size_t>(idx);
            float* pos = positionsXYZ + i * 3;
            float* vel = velocitiesXYZ + i * 3;
            const float m = (perParticleMasses && perParticleMasses[i] > 1e-12f)
                                ? perParticleMasses[i]
                                : safeDefaultMass;
            const float invM = 1.0f / m;

            ForceFieldSample s = evalAt(pos[0], pos[1], pos[2], t);
            const float fx = s.fx - damp * vel[0];
            const float fy = s.fy - damp * vel[1];
            const float fz = s.fz - damp * vel[2];

            // Symplectic (semi-implicit) Euler integration:
            //   v_{n+1} = v_n + (F(x_n, t) / m) * dt
            //   x_{n+1} = x_n + v_{n+1} * dt
            vel[0] += fx * invM * dt;
            vel[1] += fy * invM * dt;
            vel[2] += fz * invM * dt;

            pos[0] += vel[0] * dt;
            pos[1] += vel[1] * dt;
            pos[2] += vel[2] * dt;
        }
    };

    if (opts_.enableParallel && particleCount >= 64) {
        brass::codegen::parallel_for(0, static_cast<int64_t>(particleCount), stepRange, 64);
    } else {
        stepRange(0, static_cast<int64_t>(particleCount));
    }
    return particleCount;
}

size_t CompiledForceField::applyToPhysicsWorld(
    PhysicsWorld& world,
    float t,
    const std::vector<JPH::BodyID>* specificBodies) const {
    if (!ok_) return 0;

    JPH::BodyInterface& bi = world.system().GetBodyInterface();
    std::vector<JPH::BodyID> dynamicBodies;

    if (specificBodies && !specificBodies->empty()) {
        dynamicBodies.reserve(specificBodies->size());
        for (const JPH::BodyID& id : *specificBodies) {
            if (!id.IsInvalid() && bi.IsAdded(id) &&
                bi.GetMotionType(id) == JPH::EMotionType::Dynamic &&
                !world.isSensor(id)) {
                dynamicBodies.push_back(id);
            }
        }
    } else {
        JPH::BodyIDVector allBodies;
        world.system().GetBodies(allBodies);
        dynamicBodies.reserve(allBodies.size());
        for (const JPH::BodyID& id : allBodies) {
            if (!id.IsInvalid() && bi.IsAdded(id) &&
                bi.GetMotionType(id) == JPH::EMotionType::Dynamic &&
                !world.isSensor(id)) {
                dynamicBodies.push_back(id);
            }
        }
    }

    const size_t n = dynamicBodies.size();
    if (n == 0) return 0;

    std::vector<float> positions(n * 3);
    std::vector<float> velocities(n * 3);
    std::vector<float> forces(n * 3);

    for (size_t i = 0; i < n; ++i) {
        const JPH::RVec3 pos = world.getPosition(dynamicBodies[i]);
        const JPH::Vec3 vel = world.getLinearVelocity(dynamicBodies[i]);
        positions[i * 3 + 0] = static_cast<float>(pos.GetX());
        positions[i * 3 + 1] = static_cast<float>(pos.GetY());
        positions[i * 3 + 2] = static_cast<float>(pos.GetZ());
        velocities[i * 3 + 0] = vel.GetX();
        velocities[i * 3 + 1] = vel.GetY();
        velocities[i * 3 + 2] = vel.GetZ();
    }

    evalForceBatch(positions.data(), n, t, forces.data());

    const float damp = opts_.damping;
    for (size_t i = 0; i < n; ++i) {
        const float fx = forces[i * 3 + 0] - damp * velocities[i * 3 + 0];
        const float fy = forces[i * 3 + 1] - damp * velocities[i * 3 + 1];
        const float fz = forces[i * 3 + 2] - damp * velocities[i * 3 + 2];
        if (fx != 0.0f || fy != 0.0f || fz != 0.0f) {
            world.addForce(dynamicBodies[i], JPH::Vec3(fx, fy, fz));
        }
    }
    return n;
}

EnergyConservationReport CompiledForceField::verifyConservation(
    float x0,
    float y0,
    float z0,
    float vx0,
    float vy0,
    float vz0,
    float dt,
    uint32_t steps,
    float mass,
    float tolerance) const {
    EnergyConservationReport rep;
    rep.steps = steps;
    if (!ok_) return rep;

    const float m = (mass > 1e-12f) ? mass : 1.0f;
    float pos[3] = {x0, y0, z0};
    float vel[3] = {vx0, vy0, vz0};

    auto computeEnergy = [&](const float p[3], const float v[3]) -> double {
        const double ke = 0.5 * static_cast<double>(m) *
                          (static_cast<double>(v[0]) * v[0] +
                           static_cast<double>(v[1]) * v[1] +
                           static_cast<double>(v[2]) * v[2]);
        const double pe = static_cast<double>(evalPotential(p[0], p[1], p[2], 0.0f));
        return ke + pe;
    };

    rep.initialEnergy = computeEnergy(pos, vel);
    rep.minEnergy = rep.initialEnergy;
    rep.maxEnergy = rep.initialEnergy;
    const double denom = std::max(std::fabs(rep.initialEnergy), 1e-12);

    for (uint32_t step = 0; step < steps; ++step) {
        applyToParticles(pos, vel, 1, dt, m, 0.0f, nullptr);
        const double e = computeEnergy(pos, vel);
        rep.minEnergy = std::min(rep.minEnergy, e);
        rep.maxEnergy = std::max(rep.maxEnergy, e);
        const double relDrift = std::fabs(e - rep.initialEnergy) / denom;
        rep.maxRelativeDrift = std::max(rep.maxRelativeDrift, relDrift);
    }

    rep.finalEnergy = computeEnergy(pos, vel);
    rep.conserved = (rep.maxRelativeDrift <= static_cast<double>(tolerance));
    return rep;
}

}  // namespace bro::physics
