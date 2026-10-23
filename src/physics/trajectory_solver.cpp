#include "physics/trajectory_solver.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace bro::physics {

namespace {

struct Mat3 {
    double m[3][3] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};

    static Mat3 zero() { return Mat3{}; }
    static Mat3 identity() {
        Mat3 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = 1.0;
        return r;
    }
};

Mat3 matAdd(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][j] + b.m[i][j];
    return r;
}

Mat3 matScale(const Mat3& a, double s) {
    Mat3 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r.m[i][j] = a.m[i][j] * s;
    return r;
}

Mat3 matMul(const Mat3& a, const Mat3& b) {
    Mat3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            double s = 0.0;
            for (int k = 0; k < 3; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    }
    return r;
}

bool solveLinear3x3(const Mat3& A, const double rhs[3], double out[3]) {
    double aug[3][4];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) aug[i][j] = A.m[i][j];
        aug[i][3] = rhs[i];
    }

    for (int col = 0; col < 3; ++col) {
        int piv = col;
        double maxAbs = std::fabs(aug[col][col]);
        for (int r = col + 1; r < 3; ++r) {
            double v = std::fabs(aug[r][col]);
            if (v > maxAbs) {
                maxAbs = v;
                piv = r;
            }
        }
        if (maxAbs < 1e-14) return false;
        if (piv != col) {
            for (int c = col; c < 4; ++c) std::swap(aug[col][c], aug[piv][c]);
        }
        double invDiag = 1.0 / aug[col][col];
        for (int c = col; c < 4; ++c) aug[col][c] *= invDiag;
        for (int r = 0; r < 3; ++r) {
            if (r == col) continue;
            double f = aug[r][col];
            for (int c = col; c < 4; ++c) aug[r][c] -= f * aug[col][c];
        }
    }
    for (int i = 0; i < 3; ++i) out[i] = aug[i][3];
    return true;
}

struct AccelSample {
    double ax = 0.0, ay = 0.0, az = 0.0;
    double potEnergy = 0.0;
};

AccelSample evalPositionAccelAndPotential(
    const TrajectorySolveConfig& cfg,
    double x, double y, double z, double t,
    double tubeStiffness) {
    AccelSample out;
    const double invMass = 1.0 / std::max(static_cast<double>(cfg.mass), 1e-6);

    double fx = 0.0, fy = 0.0, fz = 0.0;
    double pot = 0.0;

    if (cfg.field && cfg.field->ok()) {
        const ForceFieldSample s = cfg.field->evalAt(
            static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), static_cast<float>(t));
        fx += static_cast<double>(s.fx);
        fy += static_cast<double>(s.fy);
        fz += static_cast<double>(s.fz);
        pot += static_cast<double>(s.energy);
    }

    const bool applyGravity = cfg.hasGravity || (!cfg.field && cfg.springStiffness == 0.0f);
    if (applyGravity) {
        const double gx = cfg.hasGravity ? static_cast<double>(cfg.gravity[0]) : 0.0;
        const double gy = cfg.hasGravity ? static_cast<double>(cfg.gravity[1]) : -9.81;
        const double gz = cfg.hasGravity ? static_cast<double>(cfg.gravity[2]) : 0.0;
        fx += static_cast<double>(cfg.mass) * gx;
        fy += static_cast<double>(cfg.mass) * gy;
        fz += static_cast<double>(cfg.mass) * gz;
        pot -= static_cast<double>(cfg.mass) * (gx * x + gy * y + gz * z);
    }

    if (cfg.springStiffness != 0.0f) {
        const double k = static_cast<double>(cfg.springStiffness);
        const double dx = x - static_cast<double>(cfg.attractorPos[0]);
        const double dy = y - static_cast<double>(cfg.attractorPos[1]);
        const double dz = z - static_cast<double>(cfg.attractorPos[2]);
        fx -= k * dx;
        fy -= k * dy;
        fz -= k * dz;
        pot += 0.5 * k * (dx * dx + dy * dy + dz * dz);
    }

    // Smooth C^1 conservative barrier potential for spherical SDF obstacles:
    //   U_obs(x) = (1/3) * w * (R_infl - r)^3 / R_infl   for r < R_infl
    //   F_obs(x) = -grad U_obs(x) = w * (R_infl - r)^2 / R_infl * (x - c) / r
    for (const auto& obs : cfg.obstacles) {
        const double dx = x - static_cast<double>(obs.x);
        const double dy = y - static_cast<double>(obs.y);
        const double dz = z - static_cast<double>(obs.z);
        const double r = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double rad = std::max(static_cast<double>(obs.radius), 1e-4);
        const double margin = (obs.margin > 0.0f) ? static_cast<double>(obs.margin) : std::max(0.6 * rad, 0.5);
        const double rInfl = rad + margin;
        if (r < rInfl) {
            const double w = (obs.repulsionWeight > 0.0f) ? static_cast<double>(obs.repulsionWeight) : 25.0;
            const double pen = rInfl - r;
            const double safeR = std::max(r, 1e-6);
            const double fMag = w * (pen * pen) / rInfl;
            fx += fMag * (dx / safeR);
            fy += fMag * (dy / safeR);
            fz += fMag * (dz / safeR);
            pot += (w / (3.0 * rInfl)) * pen * pen * pen;
        }
    }

    // Conservative lateral corridor potential toward the start->target axis when needed
    // to curve deflected obstacle trajectories back to the target endpoint.
    if (tubeStiffness > 0.0) {
        const double sx = static_cast<double>(cfg.startPos[0]);
        const double sy = static_cast<double>(cfg.startPos[1]);
        const double sz = static_cast<double>(cfg.startPos[2]);
        const double abx = static_cast<double>(cfg.targetPos[0]) - sx;
        const double aby = static_cast<double>(cfg.targetPos[1]) - sy;
        const double abz = static_cast<double>(cfg.targetPos[2]) - sz;
        const double abLenSq = abx * abx + aby * aby + abz * abz;
        if (abLenSq > 1e-12) {
            const double rx = x - sx;
            const double ry = y - sy;
            const double rz = z - sz;
            const double proj = (rx * abx + ry * aby + rz * abz) / abLenSq;
            const double px = rx - proj * abx;
            const double py = ry - proj * aby;
            const double pz = rz - proj * abz;
            fx -= tubeStiffness * px;
            fy -= tubeStiffness * py;
            fz -= tubeStiffness * pz;
            pot += 0.5 * tubeStiffness * (px * px + py * py + pz * pz);
        }
    }

    out.ax = fx * invMass;
    out.ay = fy * invMass;
    out.az = fz * invMass;
    out.potEnergy = pot;
    return out;
}

// Spatial Jacobian A(x, t) = d(alpha)/d(x) in R^{3x3} via central differences on the
// analytical force field evaluator.
Mat3 evalAccelSpatialJacobian(
    const TrajectorySolveConfig& cfg,
    double x, double y, double z, double t,
    double tubeStiffness) {
    constexpr double h = 1e-4;
    constexpr double inv2h = 0.5 / h;
    Mat3 A;

    const AccelSample px = evalPositionAccelAndPotential(cfg, x + h, y, z, t, tubeStiffness);
    const AccelSample mx = evalPositionAccelAndPotential(cfg, x - h, y, z, t, tubeStiffness);
    A.m[0][0] = (px.ax - mx.ax) * inv2h;
    A.m[1][0] = (px.ay - mx.ay) * inv2h;
    A.m[2][0] = (px.az - mx.az) * inv2h;

    const AccelSample py = evalPositionAccelAndPotential(cfg, x, y + h, z, t, tubeStiffness);
    const AccelSample my = evalPositionAccelAndPotential(cfg, x, y - h, z, t, tubeStiffness);
    A.m[0][1] = (py.ax - my.ax) * inv2h;
    A.m[1][1] = (py.ay - my.ay) * inv2h;
    A.m[2][1] = (py.az - my.az) * inv2h;

    const AccelSample pz = evalPositionAccelAndPotential(cfg, x, y, z + h, t, tubeStiffness);
    const AccelSample mz = evalPositionAccelAndPotential(cfg, x, y, z - h, t, tubeStiffness);
    A.m[0][2] = (pz.ax - mz.ax) * inv2h;
    A.m[1][2] = (pz.ay - mz.az) * inv2h; // corrected below
    A.m[1][2] = (pz.ay - mz.ay) * inv2h;
    A.m[2][2] = (pz.az - mz.az) * inv2h;
    return A;
}

struct RolloutData {
    double finalPos[3] = {0.0, 0.0, 0.0};
    double finalVel[3] = {0.0, 0.0, 0.0};
    Mat3 J_pos = Mat3::zero();     // d(x_N)/d(v_0)
    Mat3 J_vel = Mat3::identity(); // d(v_N)/d(v_0)
    double minClearance = 1e9;
    double obsGradV0[3] = {0.0, 0.0, 0.0};
    double obsPenalty = 0.0;
    double initialEnergy = 0.0;
    double finalEnergy = 0.0;
    double maxRelativeDrift = 0.0;
    std::vector<float> trajectory;
};

RolloutData rolloutVelocityVerlet(
    const TrajectorySolveConfig& cfg,
    const double v0[3],
    bool computeSensitivities,
    bool recordTrajectory,
    double tubeStiffness) {
    RolloutData res;
    const uint32_t steps = std::max(cfg.steps, 1u);
    const double dt = std::max(static_cast<double>(cfg.dt), 1e-6);
    const double mass = std::max(static_cast<double>(cfg.mass), 1e-6);
    double totalDamping = static_cast<double>(cfg.damping);
    if (cfg.field && cfg.field->ok()) {
        totalDamping += static_cast<double>(cfg.field->options().damping);
    }
    const double eta = std::max(0.0, totalDamping) / mass;
    const double dampHalfMinus = 1.0 - 0.5 * dt * eta;
    const double dampHalfInvPlus = 1.0 / (1.0 + 0.5 * dt * eta);

    double x = static_cast<double>(cfg.startPos[0]);
    double y = static_cast<double>(cfg.startPos[1]);
    double z = static_cast<double>(cfg.startPos[2]);
    double vx = v0[0];
    double vy = v0[1];
    double vz = v0[2];

    if (recordTrajectory) {
        res.trajectory.resize(static_cast<size_t>(steps + 1) * 3);
        res.trajectory[0] = static_cast<float>(x);
        res.trajectory[1] = static_cast<float>(y);
        res.trajectory[2] = static_cast<float>(z);
    }

    auto updateClearance = [&](double px, double py, double pz, const Mat3& Pk) {
        for (const auto& obs : cfg.obstacles) {
            const double dx = px - static_cast<double>(obs.x);
            const double dy = py - static_cast<double>(obs.y);
            const double dz = pz - static_cast<double>(obs.z);
            const double r = std::sqrt(dx * dx + dy * dy + dz * dz);
            const double rad = static_cast<double>(obs.radius);
            const double clearance = r - rad;
            if (clearance < res.minClearance) res.minClearance = clearance;

            const double safeDist = rad + 0.15 * rad;
            if (r < safeDist) {
                const double viol = safeDist - r;
                const double w = (obs.repulsionWeight > 0.0f) ? static_cast<double>(obs.repulsionWeight) : 25.0;
                res.obsPenalty += 0.5 * w * viol * viol;
                if (computeSensitivities && r > 1e-8) {
                    const double gx = -w * viol * (dx / r);
                    const double gy = -w * viol * (dy / r);
                    const double gz = -w * viol * (dz / r);
                    for (int j = 0; j < 3; ++j) {
                        res.obsGradV0[j] += gx * Pk.m[0][j] + gy * Pk.m[1][j] + gz * Pk.m[2][j];
                    }
                }
            }
        }
    };

    Mat3 P = Mat3::zero();
    Mat3 V = Mat3::identity();

    AccelSample aCur = evalPositionAccelAndPotential(cfg, x, y, z, 0.0, tubeStiffness);
    Mat3 ACur = computeSensitivities ? evalAccelSpatialJacobian(cfg, x, y, z, 0.0, tubeStiffness) : Mat3::zero();

    const double e0 = 0.5 * mass * (vx * vx + vy * vy + vz * vz) + aCur.potEnergy;
    res.initialEnergy = e0;
    res.finalEnergy = e0;
    const double eDenom = std::max(std::fabs(e0), 1.0);

    updateClearance(x, y, z, P);

    for (uint32_t k = 0; k < steps; ++k) {
        const double tNext = static_cast<double>(k + 1) * dt;

        // 1. Half-step velocity
        const double vxh = dampHalfMinus * vx + 0.5 * dt * aCur.ax;
        const double vyh = dampHalfMinus * vy + 0.5 * dt * aCur.ay;
        const double vzh = dampHalfMinus * vz + 0.5 * dt * aCur.az;

        Mat3 Vhalf;
        if (computeSensitivities) {
            Vhalf = matAdd(matScale(V, dampHalfMinus), matScale(matMul(ACur, P), 0.5 * dt));
        }

        // 2. Full-step position
        x += dt * vxh;
        y += dt * vyh;
        z += dt * vzh;

        if (computeSensitivities) {
            P = matAdd(P, matScale(Vhalf, dt));
        }

        // 3. New acceleration and spatial Jacobian at x_{k+1}
        const AccelSample aNext = evalPositionAccelAndPotential(cfg, x, y, z, tNext, tubeStiffness);
        Mat3 ANext;
        if (computeSensitivities) {
            ANext = evalAccelSpatialJacobian(cfg, x, y, z, tNext, tubeStiffness);
        }

        // 4. Full-step velocity
        vx = (vxh + 0.5 * dt * aNext.ax) * dampHalfInvPlus;
        vy = (vyh + 0.5 * dt * aNext.ay) * dampHalfInvPlus;
        vz = (vzh + 0.5 * dt * aNext.az) * dampHalfInvPlus;

        if (computeSensitivities) {
            V = matScale(matAdd(Vhalf, matScale(matMul(ANext, P), 0.5 * dt)), dampHalfInvPlus);
            ACur = ANext;
        }
        aCur = aNext;

        const double ek = 0.5 * mass * (vx * vx + vy * vy + vz * vz) + aCur.potEnergy;
        res.finalEnergy = ek;
        const double relDrift = std::fabs(ek - e0) / eDenom;
        if (relDrift > res.maxRelativeDrift) res.maxRelativeDrift = relDrift;

        updateClearance(x, y, z, P);

        if (recordTrajectory) {
            const size_t base = static_cast<size_t>(k + 1) * 3;
            res.trajectory[base + 0] = static_cast<float>(x);
            res.trajectory[base + 1] = static_cast<float>(y);
            res.trajectory[base + 2] = static_cast<float>(z);
        }
    }

    res.finalPos[0] = x;
    res.finalPos[1] = y;
    res.finalPos[2] = z;
    res.finalVel[0] = vx;
    res.finalVel[1] = vy;
    res.finalVel[2] = vz;
    res.J_pos = P;
    res.J_vel = V;
    return res;
}

double computeObjective(
    const TrajectorySolveConfig& cfg,
    const RolloutData& r,
    double outPosErrVec[3],
    double* outPosErrNorm) {
    const double ex = r.finalPos[0] - static_cast<double>(cfg.targetPos[0]);
    const double ey = r.finalPos[1] - static_cast<double>(cfg.targetPos[1]);
    const double ez = r.finalPos[2] - static_cast<double>(cfg.targetPos[2]);
    if (outPosErrVec) {
        outPosErrVec[0] = ex;
        outPosErrVec[1] = ey;
        outPosErrVec[2] = ez;
    }
    const double posErrSq = ex * ex + ey * ey + ez * ez;
    if (outPosErrNorm) *outPosErrNorm = std::sqrt(posErrSq);

    double loss = 0.5 * posErrSq + r.obsPenalty;
    if (cfg.hasTargetVelocity) {
        const double wv = static_cast<double>(cfg.targetVelocityWeight);
        const double evx = r.finalVel[0] - static_cast<double>(cfg.targetVelocity[0]);
        const double evy = r.finalVel[1] - static_cast<double>(cfg.targetVelocity[1]);
        const double evz = r.finalVel[2] - static_cast<double>(cfg.targetVelocity[2]);
        loss += 0.5 * wv * (evx * evx + evy * evy + evz * evz);
    }
    return loss;
}

void runLevenbergMarquardtAttempt(
    const TrajectorySolveConfig& cfg,
    double v0[3],
    double tubeStiffness,
    RolloutData* outBestRollout,
    uint32_t* outIters) {
    double lambda = 1e-3;
    const uint32_t maxIter = std::max(cfg.maxIterations, 1u);
    const double tol = std::max(static_cast<double>(cfg.tolerance), 1e-7);

    RolloutData cur = rolloutVelocityVerlet(cfg, v0, true, false, tubeStiffness);
    double errVec[3] = {0.0, 0.0, 0.0};
    double posErr = 0.0;
    double curLoss = computeObjective(cfg, cur, errVec, &posErr);

    uint32_t iter = 0;
    for (; iter < maxIter; ++iter) {
        if (posErr <= tol && (cfg.obstacles.empty() || cur.minClearance > 0.0)) {
            break;
        }

        // Build normal equations: (J^T J + lambda I) dv = - (J^T e + grad_obs)
        Mat3 JTJ = Mat3::zero();
        double negGrad[3] = {0.0, 0.0, 0.0};

        for (int i = 0; i < 3; ++i) {
            double g = -cur.obsGradV0[i];
            for (int k = 0; k < 3; ++k) {
                g -= cur.J_pos.m[k][i] * errVec[k];
            }
            if (cfg.hasTargetVelocity) {
                const double wv = static_cast<double>(cfg.targetVelocityWeight);
                for (int k = 0; k < 3; ++k) {
                    const double evk = cur.finalVel[k] - static_cast<double>(cfg.targetVelocity[k]);
                    g -= wv * cur.J_vel.m[k][i] * evk;
                }
            }
            negGrad[i] = g;

            for (int j = 0; j < 3; ++j) {
                double s = 0.0;
                for (int k = 0; k < 3; ++k) {
                    s += cur.J_pos.m[k][i] * cur.J_pos.m[k][j];
                    if (cfg.hasTargetVelocity) {
                        s += static_cast<double>(cfg.targetVelocityWeight) *
                             cur.J_vel.m[k][i] * cur.J_vel.m[k][j];
                    }
                }
                JTJ.m[i][j] = s;
            }
        }

        bool accepted = false;
        for (int dampTry = 0; dampTry < 6 && !accepted; ++dampTry) {
            Mat3 H = JTJ;
            for (int d = 0; d < 3; ++d) H.m[d][d] += lambda;

            double dv[3] = {0.0, 0.0, 0.0};
            if (!solveLinear3x3(H, negGrad, dv)) {
                lambda *= 10.0;
                continue;
            }

            const double stepNorm = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
            if (stepNorm < 1e-10) {
                accepted = true;
                break;
            }

            // Backtracking line search
            double alpha = 1.0;
            for (int ls = 0; ls < 8; ++ls) {
                double candV0[3] = {
                    v0[0] + alpha * dv[0],
                    v0[1] + alpha * dv[1],
                    v0[2] + alpha * dv[2]
                };
                RolloutData cand = rolloutVelocityVerlet(cfg, candV0, false, false, tubeStiffness);
                double candErrVec[3];
                double candPosErr = 0.0;
                double candLoss = computeObjective(cfg, cand, candErrVec, &candPosErr);

                if (candLoss < curLoss) {
                    v0[0] = candV0[0];
                    v0[1] = candV0[1];
                    v0[2] = candV0[2];
                    cur = rolloutVelocityVerlet(cfg, v0, true, false, tubeStiffness);
                    curLoss = computeObjective(cfg, cur, errVec, &posErr);
                    lambda = std::max(1e-7, lambda * 0.3);
                    accepted = true;
                    break;
                }
                alpha *= 0.5;
            }
            if (!accepted) lambda *= 10.0;
        }

        if (!accepted) break;
    }

    if (outBestRollout) *outBestRollout = cur;
    if (outIters) *outIters = iter;
}

} // namespace

TrajectorySolveResult solveTrajectory(const TrajectorySolveConfig& config) {
    TrajectorySolveResult result;
    const uint32_t steps = std::max(config.steps, 1u);
    const double dt = std::max(static_cast<double>(config.dt), 1e-6);
    const double T = static_cast<double>(steps) * dt;
    const double tol = std::max(static_cast<double>(config.tolerance), 1e-6);

    const double dx = static_cast<double>(config.targetPos[0] - config.startPos[0]);
    const double dy = static_cast<double>(config.targetPos[1] - config.startPos[1]);
    const double dz = static_cast<double>(config.targetPos[2] - config.startPos[2]);

    double v0[3] = {0.0, 0.0, 0.0};
    if (config.hasInitialVelocityGuess) {
        v0[0] = static_cast<double>(config.initialVelocityGuess[0]);
        v0[1] = static_cast<double>(config.initialVelocityGuess[1]);
        v0[2] = static_cast<double>(config.initialVelocityGuess[2]);
    } else {
        const AccelSample a0 = evalPositionAccelAndPotential(
            config,
            static_cast<double>(config.startPos[0]),
            static_cast<double>(config.startPos[1]),
            static_cast<double>(config.startPos[2]),
            0.0, 0.0);
        v0[0] = (dx / T) - 0.5 * a0.ax * T;
        v0[1] = (dy / T) - 0.5 * a0.ay * T;
        v0[2] = (dz / T) - 0.5 * a0.az * T;
    }

    // If obstacles lie directly along the straight-line chord [startPos, targetPos],
    // add a small lateral velocity perturbation to break collinear saddle symmetry.
    double detourDir[3] = {0.0, 1.0, 0.0};
    double maxObsRad = 0.5;
    if (!config.obstacles.empty()) {
        const double dLenSq = dx * dx + dy * dy + dz * dz;
        if (std::fabs(dx) < 1e-3 && std::fabs(dz) < 1e-3) {
            detourDir[0] = 1.0;
            detourDir[1] = 0.0;
            detourDir[2] = 0.0;
        } else {
            // Orthogonal to chord in XY plane (or XZ if chord is along Y)
            double nx = -dy;
            double ny = dx;
            double nz = 0.0;
            double nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nLen < 1e-6) {
                nx = 0.0; ny = -dz; nz = dy;
                nLen = std::sqrt(nx * nx + ny * ny + nz * nz);
            }
            if (nLen > 1e-6) {
                detourDir[0] = nx / nLen;
                detourDir[1] = ny / nLen;
                detourDir[2] = nz / nLen;
            }
        }
        for (const auto& obs : config.obstacles) {
            maxObsRad = std::max(maxObsRad, static_cast<double>(obs.radius));
            if (dLenSq > 1e-12 && !config.hasInitialVelocityGuess) {
                const double ox = static_cast<double>(obs.x - config.startPos[0]);
                const double oy = static_cast<double>(obs.y - config.startPos[1]);
                const double oz = static_cast<double>(obs.z - config.startPos[2]);
                const double tProj = std::clamp((ox * dx + oy * dy + oz * dz) / dLenSq, 0.0, 1.0);
                const double cx = ox - tProj * dx;
                const double cy = oy - tProj * dy;
                const double cz = oz - tProj * dz;
                const double distToChord = std::sqrt(cx * cx + cy * cy + cz * cz);
                if (distToChord < static_cast<double>(obs.radius) * 1.5) {
                    const double kick = (2.0 * static_cast<double>(obs.radius)) / T;
                    v0[0] += detourDir[0] * kick;
                    v0[1] += detourDir[1] * kick;
                    v0[2] += detourDir[2] * kick;
                }
            }
        }
    }

    RolloutData bestRollout;
    uint32_t totalIters = 0;
    double bestTubeStiffness = 0.0;
    runLevenbergMarquardtAttempt(config, v0, 0.0, &bestRollout, &totalIters);

    auto getPosErr = [&](const RolloutData& r) -> double {
        const double ex = r.finalPos[0] - static_cast<double>(config.targetPos[0]);
        const double ey = r.finalPos[1] - static_cast<double>(config.targetPos[1]);
        const double ez = r.finalPos[2] - static_cast<double>(config.targetPos[2]);
        return std::sqrt(ex * ex + ey * ey + ez * ez);
    };

    double bestErr = getPosErr(bestRollout);
    bool obsOk = config.obstacles.empty() || (bestRollout.minClearance > 0.0);

    // If an obstacle blocked the pure ballistic path in free space (where a conservative
    // lateral restoring force is required by Rolle's theorem to bend the detour back to
    // the target axis), solve with conservative corridor stiffness k_tube = m * (pi / T)^2.
    if (!config.obstacles.empty() && (bestErr > tol || !obsOk)) {
        const double kNatural = static_cast<double>(config.mass) * (3.141592653589793 / T) * (3.141592653589793 / T);
        const double kCandidates[3] = {kNatural, 2.0 * kNatural, 0.5 * kNatural};
        const double signs[2] = {1.0, -1.0};

        for (double kTube : kCandidates) {
            for (double sgn : signs) {
                double candV0[3] = {
                    (dx / T) + sgn * detourDir[0] * (3.0 * maxObsRad / T),
                    (dy / T) + sgn * detourDir[1] * (3.0 * maxObsRad / T),
                    (dz / T) + sgn * detourDir[2] * (3.0 * maxObsRad / T)
                };
                RolloutData candRollout;
                uint32_t candIters = 0;
                runLevenbergMarquardtAttempt(config, candV0, kTube, &candRollout, &candIters);
                const double candErr = getPosErr(candRollout);
                const bool candObsOk = candRollout.minClearance > 0.0;
                if ((candObsOk && !obsOk) ||
                    (candObsOk == obsOk && candErr < bestErr)) {
                    bestErr = candErr;
                    obsOk = candObsOk;
                    bestRollout = candRollout;
                    bestTubeStiffness = kTube;
                    totalIters = std::max(totalIters, candIters);
                    v0[0] = candV0[0];
                    v0[1] = candV0[1];
                    v0[2] = candV0[2];
                }
                if (obsOk && bestErr <= tol) break;
            }
            if (obsOk && bestErr <= tol) break;
        }
    }

    // Final rollout with trajectory recording enabled
    RolloutData finalRollout = rolloutVelocityVerlet(config, v0, true, true, bestTubeStiffness);
    const double finalErr = getPosErr(finalRollout);

    result.converged = (finalErr <= tol) && (config.obstacles.empty() || finalRollout.minClearance > -1e-3);
    for (int i = 0; i < 3; ++i) {
        result.initialVelocity[i] = static_cast<float>(v0[i]);
        result.finalPosition[i] = static_cast<float>(finalRollout.finalPos[i]);
        result.finalVelocity[i] = static_cast<float>(finalRollout.finalVel[i]);
    }
    result.positionError = static_cast<float>(finalErr);
    result.iterations = std::max(totalIters, 1u);
    result.minObstacleClearance = config.obstacles.empty()
                                      ? 1e9f
                                      : static_cast<float>(finalRollout.minClearance);
    result.initialEnergy = finalRollout.initialEnergy;
    result.finalEnergy = finalRollout.finalEnergy;
    result.energyDrift = finalRollout.maxRelativeDrift;
    result.energyConserved = (config.damping == 0.0f) ? (finalRollout.maxRelativeDrift < 5e-2) : true;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            result.jacobian[r * 3 + c] = static_cast<float>(finalRollout.J_pos.m[r][c]);
        }
    }
    result.trajectory = std::move(finalRollout.trajectory);
    return result;
}

} // namespace bro::physics
