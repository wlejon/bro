#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "physics/force_field_jit.h"

namespace bro::physics {

struct TrajectoryObstacle {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float radius = 0.5f;
    float repulsionWeight = 25.0f;
    float margin = 0.5f;
};

struct TrajectorySolveConfig {
    const CompiledForceField* field = nullptr;

    float startPos[3] = {0.0f, 0.0f, 0.0f};
    float targetPos[3] = {1.0f, 0.0f, 0.0f};

    bool hasTargetVelocity = false;
    float targetVelocity[3] = {0.0f, 0.0f, 0.0f};
    float targetVelocityWeight = 0.1f;

    bool hasInitialVelocityGuess = false;
    float initialVelocityGuess[3] = {0.0f, 0.0f, 0.0f};

    bool hasGravity = false;
    float gravity[3] = {0.0f, -9.81f, 0.0f};

    float springStiffness = 0.0f;
    float attractorPos[3] = {0.0f, 0.0f, 0.0f};

    float dt = 0.01f;
    uint32_t steps = 100;
    float mass = 1.0f;
    float damping = 0.0f;

    float tolerance = 1e-4f;
    uint32_t maxIterations = 40;

    std::vector<TrajectoryObstacle> obstacles;
};

struct TrajectorySolveResult {
    bool converged = false;
    float initialVelocity[3] = {0.0f, 0.0f, 0.0f};
    float finalPosition[3] = {0.0f, 0.0f, 0.0f};
    float finalVelocity[3] = {0.0f, 0.0f, 0.0f};
    float positionError = 0.0f;
    uint32_t iterations = 0;
    float minObstacleClearance = 1e9f;
    double initialEnergy = 0.0;
    double finalEnergy = 0.0;
    double energyDrift = 0.0;
    bool energyConserved = true;
    float jacobian[9] = {0.0f}; // Row-major 3x3 sensitivity matrix d(x_N)/d(v_0)
    std::vector<float> trajectory; // (steps + 1) * 3 packed [x, y, z] positions
};

// Propagates a symplectic Velocity-Verlet / Leapfrog rollout with analytical 3x3
// tangent sensitivity matrix J = d(x_N)/d(v_0) and solves for the launch velocity
// v_0 via damped Levenberg-Marquardt with backtracking line search.
TrajectorySolveResult solveTrajectory(const TrajectorySolveConfig& config);

} // namespace bro::physics
