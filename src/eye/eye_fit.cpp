#include "eye/eye_fit.h"
#include "eye/eye_measure.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

namespace bro::eye {
namespace {

struct SplitMix64 {
    uint64_t state;
    explicit SplitMix64(uint64_t seed) : state(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
    uint64_t nextU64() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double uniform01() {
        return static_cast<double>(nextU64() >> 11) * (1.0 / 9007199254740992.0);
    }
    double normal01() {
        const double u1 = std::max(1e-12, uniform01());
        const double u2 = uniform01();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.28318530717958647692 * u2);
    }
};

double toUnit(double v, const EyeFitParam& p) {
    if (p.scale == "log" && p.min > 0.0 && p.max > p.min) {
        const double lv = std::log(std::clamp(v, p.min, p.max));
        const double lmin = std::log(p.min);
        const double lmax = std::log(p.max);
        return std::clamp((lv - lmin) / (lmax - lmin), 0.0, 1.0);
    }
    if (p.max <= p.min) return 0.5;
    return std::clamp((v - p.min) / (p.max - p.min), 0.0, 1.0);
}

double fromUnit(double u, const EyeFitParam& p) {
    u = std::clamp(u, 0.0, 1.0);
    if (p.scale == "log" && p.min > 0.0 && p.max > p.min) {
        const double lmin = std::log(p.min);
        const double lmax = std::log(p.max);
        return std::exp(lmin + u * (lmax - lmin));
    }
    return p.min + u * (p.max - p.min);
}

double readMetric(const EyeMeasurement& m, const std::string& name, double& defaultScale) {
    if (name == "lumaMean") { defaultScale = 0.15; return m.lumaMean; }
    if (name == "lumaStd") { defaultScale = 0.10; return m.lumaStd; }
    if (name == "lumaP50") { defaultScale = 0.15; return m.lumaP50; }
    if (name == "dynamicRange") { defaultScale = 0.25; return m.dynamicRange; }
    if (name == "exposureBias") { defaultScale = 0.15; return m.exposureBias; }
    if (name == "crushedBlackRatio") { defaultScale = 0.10; return m.crushedBlackRatio; }
    if (name == "blownWhiteRatio") { defaultScale = 0.10; return m.blownWhiteRatio; }
    if (name == "bandingScore") { defaultScale = 0.20; return m.bandingScore; }
    if (name == "chromaMean") { defaultScale = 0.05; return m.chromaMean; }
    if (name == "chromaP95") { defaultScale = 0.08; return m.chromaP95; }
    if (name == "colorfulRatio") { defaultScale = 0.25; return m.colorfulRatio; }
    if (name == "hueEntropy") { defaultScale = 0.25; return m.hueEntropy; }
    if (name == "warmCoolBalance") { defaultScale = 0.30; return m.warmCoolBalance; }
    if (name == "sharpness") { defaultScale = 0.25; return m.sharpness; }
    if (name == "edgeDensity") { defaultScale = 0.10; return m.edgeDensity; }
    if (name == "clutterScore") { defaultScale = 0.25; return m.clutterScore; }
    if (name == "whitespaceRatio") { defaultScale = 0.25; return m.whitespaceRatio; }
    if (name == "tiling" || name == "tilingScore") { defaultScale = 0.25; return m.tiling.score; }
    if (name == "centerOfMassX") { defaultScale = 0.15; return m.balance.centerOfMassX; }
    if (name == "centerOfMassY") { defaultScale = 0.15; return m.balance.centerOfMassY; }
    defaultScale = 1.0;
    return 0.0;
}

// Symmetric Jacobi eigendecomposition: C = B * diag(D)^2 * B^T (all sizes n x n).
void jacobiEigen(int n, std::vector<double> A, std::vector<double>& B, std::vector<double>& D) {
    B.assign(static_cast<size_t>(n) * n, 0.0);
    for (int i = 0; i < n; ++i) B[static_cast<size_t>(i) * n + i] = 1.0;
    D.resize(n);

    for (int iter = 0; iter < 50 * n * n; ++iter) {
        int p = 0, q = 1;
        double maxOff = 0.0;
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                const double v = std::abs(A[static_cast<size_t>(i) * n + j]);
                if (v > maxOff) { maxOff = v; p = i; q = j; }
            }
        }
        if (maxOff < 1e-12) break;

        const double app = A[static_cast<size_t>(p) * n + p];
        const double aqq = A[static_cast<size_t>(q) * n + q];
        const double apq = A[static_cast<size_t>(p) * n + q];
        const double tau = (aqq - app) / (2.0 * apq);
        const double t = (tau >= 0.0) ? (1.0 / (tau + std::sqrt(1.0 + tau * tau)))
                                      : (-1.0 / (-tau + std::sqrt(1.0 + tau * tau)));
        const double c = 1.0 / std::sqrt(1.0 + t * t);
        const double s = t * c;

        A[static_cast<size_t>(p) * n + p] = app - t * apq;
        A[static_cast<size_t>(q) * n + q] = aqq + t * apq;
        A[static_cast<size_t>(p) * n + q] = 0.0;
        A[static_cast<size_t>(q) * n + p] = 0.0;

        for (int r = 0; r < n; ++r) {
            if (r == p || r == q) continue;
            const double arp = A[static_cast<size_t>(r) * n + p];
            const double arq = A[static_cast<size_t>(r) * n + q];
            A[static_cast<size_t>(r) * n + p] = A[static_cast<size_t>(p) * n + r] = c * arp - s * arq;
            A[static_cast<size_t>(r) * n + q] = A[static_cast<size_t>(q) * n + r] = s * arp + c * arq;
        }
        for (int r = 0; r < n; ++r) {
            const double brp = B[static_cast<size_t>(r) * n + p];
            const double brq = B[static_cast<size_t>(r) * n + q];
            B[static_cast<size_t>(r) * n + p] = c * brp - s * brq;
            B[static_cast<size_t>(r) * n + q] = s * brp + c * brq;
        }
    }
    for (int i = 0; i < n; ++i) {
        D[i] = std::sqrt(std::max(1e-12, A[static_cast<size_t>(i) * n + i]));
    }
}

}  // namespace

EyeFitResult fitVisualParams(
    const EyeFitOptions& opts,
    const std::function<EyeFitCandidateResult(const std::vector<double>& values)>& evalFn,
    const std::function<void(int eval, double bestDist, const std::vector<double>& bestVals)>& progressFn) {
    EyeFitResult out;
    const int n = static_cast<int>(opts.params.size());
    if (n <= 0 || !evalFn) return out;

    const int maxEval = std::max(1, opts.maxEvaluations);
    SplitMix64 rng(opts.seed);

    double bestDist = std::numeric_limits<double>::infinity();
    std::vector<double> bestVals(n, 0.0);
    EyeMeasurement bestFrame;
    EyeComparison bestCmp;
    bool bestHasCmp = false;

    auto scoreCandidate = [&](const std::vector<double>& vals, double& outDist, EyeMeasurement& outFrame,
                              EyeComparison& outCmp, bool& outHasCmp) -> bool {
        EyeFitCandidateResult cand = evalFn(vals);
        if (!cand.ok || cand.image.empty()) {
            outDist = 1e9;
            return false;
        }
        EyeMeasureOptions mo;
        mo.detectTiling = false;
        for (const auto& mt : opts.measures) {
            if (mt.name == "tiling" || mt.name == "tilingScore") mo.detectTiling = true;
        }
        outFrame = measureImage(cand.image.view(), mo);

        double d = 0.0;
        if (opts.hasReference && !opts.reference.empty()) {
            outCmp = compareImages(cand.image.view(), opts.reference.view());
            outHasCmp = true;
            d += outCmp.score * opts.referenceWeight;
        } else {
            outHasCmp = false;
        }
        for (const auto& mt : opts.measures) {
            double defScale = 1.0;
            const double actual = readMetric(outFrame, mt.name, defScale);
            const double scale = mt.scale > 0.0 ? mt.scale : defScale;
            d += (std::abs(actual - mt.target) / scale) * mt.weight;
        }
        if (cand.hasAudit) {
            d += cand.auditErrors * opts.auditErrorWeight + cand.auditWarnings * opts.auditWarningWeight;
        }
        if (cand.hasCustomScore) {
            d += cand.customScore * opts.customScorerWeight;
        }
        outDist = d;
        return true;
    };

    int evals = 0;
    int lambdaBase = 4 + static_cast<int>(std::floor(3.0 * std::log(static_cast<double>(n))));
    int popMult = 1;

    // Evaluate explicit starting point first.
    {
        std::vector<double> x0(n);
        for (int i = 0; i < n; ++i) {
            const auto& p = opts.params[i];
            x0[i] = p.hasStart ? std::clamp(p.start, p.min, p.max) : fromUnit(0.5, p);
        }
        double d = 0.0;
        EyeMeasurement mf;
        EyeComparison mc;
        bool hasC = false;
        scoreCandidate(x0, d, mf, mc, hasC);
        ++evals;
        bestDist = d;
        bestVals = x0;
        bestFrame = std::move(mf);
        bestCmp = std::move(mc);
        bestHasCmp = hasC;
        out.history.push_back(bestDist);
        if (progressFn) progressFn(evals, bestDist, bestVals);
    }

    while (evals < maxEval && bestDist > opts.targetDistance) {
        const int lambda = std::max(4, lambdaBase * popMult);
        const int mu = std::max(1, lambda / 2);

        std::vector<double> weights(mu);
        double wSum = 0.0;
        for (int i = 0; i < mu; ++i) {
            weights[i] = std::log(mu + 0.5) - std::log(static_cast<double>(i + 1));
            wSum += weights[i];
        }
        double w2Sum = 0.0;
        for (int i = 0; i < mu; ++i) {
            weights[i] /= wSum;
            w2Sum += weights[i] * weights[i];
        }
        const double muEff = 1.0 / w2Sum;
        const double cc = (4.0 + muEff / n) / (n + 4.0 + 2.0 * muEff / n);
        const double cs = (muEff + 2.0) / (n + muEff + 5.0);
        const double c1 = 2.0 / ((n + 1.3) * (n + 1.3) + muEff);
        const double cmu = std::min(1.0 - c1, 2.0 * (muEff - 2.0 + 1.0 / muEff) / ((n + 2.0) * (n + 2.0) + muEff));
        const double damps = 1.0 + 2.0 * std::max(0.0, std::sqrt((muEff - 1.0) / (n + 1.0)) - 1.0) + cs;
        const double chiN = std::sqrt(static_cast<double>(n)) * (1.0 - 1.0 / (4.0 * n) + 1.0 / (21.0 * n * n));

        std::vector<double> meanU(n);
        for (int i = 0; i < n; ++i) {
            meanU[i] = (popMult == 1 && opts.params[i].hasStart) ? toUnit(opts.params[i].start, opts.params[i])
                                                                : (popMult == 1 ? 0.5 : (0.15 + 0.70 * rng.uniform01()));
        }
        double sigma = 0.25;
        std::vector<double> pc(n, 0.0), ps(n, 0.0);
        std::vector<double> C(static_cast<size_t>(n) * n, 0.0);
        std::vector<double> B(static_cast<size_t>(n) * n, 0.0);
        std::vector<double> D(n, 1.0);
        for (int i = 0; i < n; ++i) {
            C[static_cast<size_t>(i) * n + i] = 1.0;
            B[static_cast<size_t>(i) * n + i] = 1.0;
        }

        double runBest = bestDist;
        int stallGens = 0;
        const int maxStallGens = 12 + (20 * n) / lambda;

        while (evals < maxEval && bestDist > opts.targetDistance) {
            if (n <= 32) {
                jacobiEigen(n, C, B, D);
            } else {
                for (int i = 0; i < n; ++i) D[i] = std::sqrt(std::max(1e-12, C[static_cast<size_t>(i) * n + i]));
            }

            struct Cand {
                std::vector<double> u;
                std::vector<double> z;
                double dist = 1e9;
            };
            std::vector<Cand> pop;
            pop.reserve(lambda);

            for (int k = 0; k < lambda && evals < maxEval; ++k) {
                Cand cand;
                cand.u.resize(n);
                cand.z.resize(n);
                std::vector<double> y(n, 0.0);
                for (int i = 0; i < n; ++i) cand.z[i] = rng.normal01();
                if (n <= 32) {
                    for (int r = 0; r < n; ++r) {
                        double s = 0.0;
                        for (int c = 0; c < n; ++c) s += B[static_cast<size_t>(r) * n + c] * D[c] * cand.z[c];
                        y[r] = s;
                    }
                } else {
                    for (int r = 0; r < n; ++r) y[r] = D[r] * cand.z[r];
                }
                std::vector<double> vals(n);
                for (int i = 0; i < n; ++i) {
                    cand.u[i] = std::clamp(meanU[i] + sigma * y[i], 0.0, 1.0);
                    vals[i] = fromUnit(cand.u[i], opts.params[i]);
                }

                EyeMeasurement mf;
                EyeComparison mc;
                bool hasC = false;
                scoreCandidate(vals, cand.dist, mf, mc, hasC);
                ++evals;

                if (cand.dist < bestDist) {
                    bestDist = cand.dist;
                    bestVals = vals;
                    bestFrame = std::move(mf);
                    bestCmp = std::move(mc);
                    bestHasCmp = hasC;
                }
                pop.push_back(std::move(cand));
                if (bestDist <= opts.targetDistance) break;
            }

            if (pop.empty()) break;
            std::sort(pop.begin(), pop.end(), [](const Cand& a, const Cand& b) { return a.dist < b.dist; });
            out.history.push_back(bestDist);
            if (progressFn) progressFn(evals, bestDist, bestVals);

            if (pop[0].dist < runBest * 0.99) {
                runBest = pop[0].dist;
                stallGens = 0;
            } else {
                ++stallGens;
            }
            if (stallGens >= maxStallGens || sigma < 1e-6 || pop.size() < static_cast<size_t>(mu)) {
                break;
            }

            std::vector<double> oldMean = meanU;
            std::fill(meanU.begin(), meanU.end(), 0.0);
            for (int k = 0; k < mu; ++k) {
                for (int i = 0; i < n; ++i) meanU[i] += weights[k] * pop[k].u[i];
            }

            const double oldSigma = sigma;
            std::vector<double> yW(n);
            for (int i = 0; i < n; ++i) yW[i] = (meanU[i] - oldMean[i]) / oldSigma;

            // ps update
            double psNorm2 = 0.0;
            const double psFactor = std::sqrt(cs * (2.0 - cs) * muEff);
            for (int i = 0; i < n; ++i) {
                double cinvY = 0.0;
                if (n <= 32) {
                    for (int k = 0; k < n; ++k) {
                        double btY = 0.0;
                        for (int r = 0; r < n; ++r) btY += B[static_cast<size_t>(r) * n + k] * yW[r];
                        cinvY += B[static_cast<size_t>(i) * n + k] * (btY / std::max(1e-9, D[k]));
                    }
                } else {
                    cinvY = yW[i] / std::max(1e-9, D[i]);
                }
                ps[i] = (1.0 - cs) * ps[i] + psFactor * cinvY;
                psNorm2 += ps[i] * ps[i];
            }
            const double psNorm = std::sqrt(psNorm2);
            sigma *= std::exp((cs / damps) * (psNorm / chiN - 1.0));
            sigma = std::clamp(sigma, 1e-6, 0.5);

            const double pcFactor = std::sqrt(cc * (2.0 - cc) * muEff);
            for (int i = 0; i < n; ++i) pc[i] = (1.0 - cc) * pc[i] + pcFactor * yW[i];

            const double cBase = 1.0 - c1 - cmu;
            for (int r = 0; r < n; ++r) {
                for (int c = r; c < n; ++c) {
                    double rankMu = 0.0;
                    for (int k = 0; k < mu; ++k) {
                        const double yr = (pop[k].u[r] - oldMean[r]) / std::max(1e-6, oldSigma);
                        const double yc = (pop[k].u[c] - oldMean[c]) / std::max(1e-6, oldSigma);
                        rankMu += weights[k] * yr * yc;
                    }
                    const double v = cBase * C[static_cast<size_t>(r) * n + c] + c1 * pc[r] * pc[c] + cmu * rankMu;
                    C[static_cast<size_t>(r) * n + c] = C[static_cast<size_t>(c) * n + r] = v;
                }
            }
        }
        popMult = std::min(popMult * 2, 4);
    }

    out.ok = std::isfinite(bestDist);
    out.distance = bestDist;
    out.bestValues = std::move(bestVals);
    out.evaluations = evals;
    out.frame = std::move(bestFrame);
    out.comparison = std::move(bestCmp);
    out.hasComparison = bestHasCmp;
    return out;
}

}  // namespace bro::eye
