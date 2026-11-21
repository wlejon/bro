#include "eye/eye_visualize.h"
#include "eye/eye_color.h"
#include "eye/eye_draw_internal.h"
#include "eye/eye_measure.h"
#include "eye/eye_visualize_taste.h"

#include <broimage/encode.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace bro::eye {
using draw::assembleSheet;
using draw::blitPanel;
using draw::drawRectBox;
using draw::drawText;
using draw::fillRect;
using draw::fitPanel;

EyeVisualResult visualizeImages(const std::vector<EyeImageView>& images, const EyeVisualizeOptions& opts) {
    EyeVisualResult res;
    if (images.empty() || !images[0].valid()) return res;

    const std::string mode = opts.mode.empty() ? (images.size() >= 2 ? "diff" : "exposure") : opts.mode;
    const int fscale = std::clamp(opts.fontScale, 1, 4);

    if (visualizeTasteMode(mode, images, opts, res)) {
        // Fall through to optional PNG encoding below
    } else if (mode == "diff" && images.size() >= 2 && images[1].valid()) {
        EyeImage pa = fitPanel(images[0], opts.maxPanelDim);
        EyeImage pb = resampleRgba(images[1], pa.width, pa.height);
        EyeImage heat{pa.width, pa.height, std::vector<uint8_t>(static_cast<size_t>(pa.width) * pa.height * 4)};

        double deSum = 0.0;
        const size_t n = static_cast<size_t>(pa.width) * pa.height;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* a = &pa.rgba[i * 4];
            const uint8_t* b = &pb.rgba[i * 4];
            const double de = color::oklabDeltaE(color::srgbToOklab(a[0], a[1], a[2]),
                                                 color::srgbToOklab(b[0], b[1], b[2]));
            deSum += de;
            const auto c = color::magmaColor(std::clamp(de / 40.0, 0.0, 1.0));
            heat.rgba[i * 4 + 0] = c.r;
            heat.rgba[i * 4 + 1] = c.g;
            heat.rgba[i * 4 + 2] = c.b;
            heat.rgba[i * 4 + 3] = 255;
        }
        const auto cmp = compareImages(images[0], images[1]);
        char diffLbl[96];
        std::snprintf(diffLbl, sizeof(diffLbl), "deltaE %.1f | SSIM %.3f | score %.3f",
                      deSum / std::max<size_t>(1, n), cmp.ssim, cmp.score);
        std::vector<std::string> lbls = {
            !opts.labels.empty() ? opts.labels[0] : "frame",
            opts.labels.size() > 1 ? opts.labels[1] : "reference",
            diffLbl,
        };
        res = assembleSheet({std::move(pa), std::move(pb), std::move(heat)}, lbls, opts.layout, fscale);
    } else if (mode == "exposure") {
        EyeImage base = fitPanel(images[0], opts.maxPanelDim);
        EyeImage expo{base.width, base.height, std::vector<uint8_t>(static_cast<size_t>(base.width) * base.height * 4)};
        const size_t n = static_cast<size_t>(base.width) * base.height;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t* p = &base.rgba[i * 4];
            const auto lab = color::srgbToOklab(p[0], p[1], p[2]);
            uint8_t r = 0, g = 0, b = 0;
            if ((p[0] <= 4 && p[1] <= 4 && p[2] <= 4) || lab.L < 0.04) {
                r = 35; g = 90; b = 255;   // crushed black -> blue
            } else if ((p[0] >= 251 && p[1] >= 251 && p[2] >= 251) || lab.L > 0.97) {
                r = 255; g = 55; b = 55;   // blown white -> red
            } else if (lab.L >= 0.42 && lab.L <= 0.58) {
                r = 70; g = 210; b = 95;   // middle grey -> green
            } else {
                const uint8_t y = static_cast<uint8_t>(std::clamp(std::lround(lab.L * 255.0), 0L, 255L));
                r = y; g = y; b = y;
            }
            expo.rgba[i * 4 + 0] = r;
            expo.rgba[i * 4 + 1] = g;
            expo.rgba[i * 4 + 2] = b;
            expo.rgba[i * 4 + 3] = 255;
        }
        const auto m = measureImage(images[0], {3, 3, 3, false});
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "L %.2f | crushed %.1f%% | blown %.1f%%",
                      m.lumaMean, m.crushedBlackRatio * 100.0, m.blownWhiteRatio * 100.0);
        res = assembleSheet({std::move(base), std::move(expo)},
                            {!opts.labels.empty() ? opts.labels[0] : "frame", lbl}, opts.layout, fscale);
    } else if (mode == "tiling" || mode == "tile2x2") {
        EyeImage base = fitPanel(images[0], opts.maxPanelDim);
        const int fftN = 128;
        std::vector<float> power;
        EyeTiling tStats{};
        computeFftPowerMap(images[0], fftN, power, &tStats);
        const int outSide = std::max(fftN, std::min(base.width, base.height));
        EyeImage fftImg{outSide, outSide, std::vector<uint8_t>(static_cast<size_t>(outSide) * outSide * 4, 255)};
        for (int y = 0; y < outSide; ++y) {
            const int sy = (y * fftN) / outSide;
            for (int x = 0; x < outSide; ++x) {
                const int sx = (x * fftN) / outSide;
                const auto c = color::magmaColor(power[static_cast<size_t>(sy) * fftN + sx]);
                const size_t idx = (static_cast<size_t>(y) * outSide + x) * 4;
                fftImg.rgba[idx + 0] = c.r;
                fftImg.rgba[idx + 1] = c.g;
                fftImg.rgba[idx + 2] = c.b;
                fftImg.rgba[idx + 3] = 255;
            }
        }
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "FFT tiling %.2f (peak %.1fx)", tStats.score, tStats.peakRatio);
        if (mode == "tile2x2") {
            const int qMax = std::max(64, opts.maxPanelDim / 2);
            EyeImage quad = fitPanel(images[0], qMax);
            const int tw = quad.width * 2;
            const int th = quad.height * 2;
            EyeImage tiled2x2{tw, th, std::vector<uint8_t>(static_cast<size_t>(tw) * th * 4, 255)};
            blitPanel(tiled2x2, 0, 0, quad);
            blitPanel(tiled2x2, quad.width, 0, quad);
            blitPanel(tiled2x2, 0, quad.height, quad);
            blitPanel(tiled2x2, quad.width, quad.height, quad);
            // Outer-margin cyan seam tick marks only so interior wrap boundary stays unobstructed.
            const int tick = 8;
            fillRect(tiled2x2, quad.width - 1, 0, 2, tick, 40, 220, 240);
            fillRect(tiled2x2, quad.width - 1, th - tick, 2, tick, 40, 220, 240);
            fillRect(tiled2x2, 0, quad.height - 1, tick, 2, 40, 220, 240);
            fillRect(tiled2x2, tw - tick, quad.height - 1, tick, 2, 40, 220, 240);

            const auto m = measureImage(images[0], {1, 1, 0, false});
            char tileLbl[96];
            std::snprintf(tileLbl, sizeof(tileLbl), "%s | seam %.2f (dE %.1f)",
                          !opts.labels.empty() ? opts.labels[0].c_str() : "tile 2x2",
                          m.seam.score, m.seam.edgeDeltaE);
            res = assembleSheet({std::move(tiled2x2), std::move(fftImg)},
                                {tileLbl, opts.labels.size() > 1 ? opts.labels[1] : lbl},
                                opts.layout, fscale);
        } else {
            res = assembleSheet({std::move(base), std::move(fftImg)},
                                {!opts.labels.empty() ? opts.labels[0] : "frame", lbl}, opts.layout, fscale);
        }
    } else if ((mode == "depth" || mode == "normals") && opts.depth && opts.depthWidth > 0 && opts.depthHeight > 0) {
        const int dw = opts.depthWidth, dh = opts.depthHeight;
        EyeImage rawDepth{dw, dh, std::vector<uint8_t>(static_cast<size_t>(dw) * dh * 4, 255)};
        if (mode == "depth") {
            float dMin = 1.0f, dMax = 0.0f;
            for (size_t i = 0; i < static_cast<size_t>(dw) * dh; ++i) {
                const float d = opts.depth[i];
                if (d > 1e-7f) { dMin = std::min(dMin, d); dMax = std::max(dMax, d); }
            }
            const double span = std::max(1e-6, static_cast<double>(dMax - dMin));
            for (size_t i = 0; i < static_cast<size_t>(dw) * dh; ++i) {
                const float d = opts.depth[i];
                if (!(d > 1e-7f)) {
                    rawDepth.rgba[i * 4 + 0] = 16; rawDepth.rgba[i * 4 + 1] = 22; rawDepth.rgba[i * 4 + 2] = 36;
                } else {
                    const double t = std::clamp((d - dMin) / span, 0.0, 1.0);
                    const auto c = color::magmaColor(t);
                    rawDepth.rgba[i * 4 + 0] = c.r; rawDepth.rgba[i * 4 + 1] = c.g; rawDepth.rgba[i * 4 + 2] = c.b;
                }
            }
        } else {
            // Reconstruct view-space normals via invProj unprojection when available so
            // metric surface detail remains sharp at any camera distance.
            auto unproj = [&](int x, int y, float d, double out[3]) -> bool {
                if (!(d > 1e-7f)) return false;
                if (!opts.hasCamera) {
                    out[0] = (x + 0.5) / dw;
                    out[1] = -(y + 0.5) / dh;
                    out[2] = d * 60.0;
                    return true;
                }
                const double cx = 2.0 * (x + 0.5) / dw - 1.0;
                const double cy = 2.0 * (y + 0.5) / dh - 1.0;
                const auto& m = opts.invProj;
                double v[4];
                for (int r = 0; r < 4; ++r) {
                    v[r] = m[r] * cx + m[4 + r] * cy + m[8 + r] * d + m[12 + r];
                }
                if (std::abs(v[3]) < 1e-9) return false;
                out[0] = v[0] / v[3];
                out[1] = v[1] / v[3];
                out[2] = v[2] / v[3];
                return true;
            };
            for (int y = 0; y < dh; ++y) {
                for (int x = 0; x < dw; ++x) {
                    const size_t idx = static_cast<size_t>(y) * dw + x;
                    double pc[3];
                    if (!unproj(x, y, opts.depth[idx], pc)) {
                        rawDepth.rgba[idx * 4 + 0] = 20; rawDepth.rgba[idx * 4 + 1] = 24; rawDepth.rgba[idx * 4 + 2] = 34;
                        continue;
                    }
                    double pr[3], pu[3];
                    const bool hasR = (x + 1 < dw) && unproj(x + 1, y, opts.depth[idx + 1], pr);
                    const bool hasL = !hasR && (x > 0) && unproj(x - 1, y, opts.depth[idx - 1], pr);
                    const bool hasU = (y > 0) && unproj(x, y - 1, opts.depth[idx - dw], pu);
                    const bool hasD = !hasU && (y + 1 < dh) && unproj(x, y + 1, opts.depth[idx + dw], pu);
                    if ((!hasR && !hasL) || (!hasU && !hasD)) {
                        rawDepth.rgba[idx * 4 + 0] = 128; rawDepth.rgba[idx * 4 + 1] = 128; rawDepth.rgba[idx * 4 + 2] = 255;
                        continue;
                    }
                    double dpx[3] = {
                        hasR ? (pr[0] - pc[0]) : (pc[0] - pr[0]),
                        hasR ? (pr[1] - pc[1]) : (pc[1] - pr[1]),
                        hasR ? (pr[2] - pc[2]) : (pc[2] - pr[2]),
                    };
                    double dpy[3] = {
                        hasU ? (pu[0] - pc[0]) : (pc[0] - pu[0]),
                        hasU ? (pu[1] - pc[1]) : (pc[1] - pu[1]),
                        hasU ? (pu[2] - pc[2]) : (pc[2] - pu[2]),
                    };
                    // Cross(right, up) points toward camera (+Z in view space)
                    double nx = dpx[1] * dpy[2] - dpx[2] * dpy[1];
                    double ny = dpx[2] * dpy[0] - dpx[0] * dpy[2];
                    double nz = dpx[0] * dpy[1] - dpx[1] * dpy[0];
                    if (nz < 0.0) { nx = -nx; ny = -ny; nz = -nz; }
                    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
                    if (len < 1e-12) {
                        nx = 0.0; ny = 0.0; nz = 1.0;
                    } else {
                        nx /= len; ny /= len; nz /= len;
                    }
                    rawDepth.rgba[idx * 4 + 0] = static_cast<uint8_t>(std::clamp(std::lround((nx * 0.5 + 0.5) * 255.0), 0L, 255L));
                    rawDepth.rgba[idx * 4 + 1] = static_cast<uint8_t>(std::clamp(std::lround((ny * 0.5 + 0.5) * 255.0), 0L, 255L));
                    rawDepth.rgba[idx * 4 + 2] = static_cast<uint8_t>(std::clamp(std::lround((nz * 0.5 + 0.5) * 255.0), 0L, 255L));
                }
            }
        }
        EyeImage base = fitPanel(images[0], opts.maxPanelDim);
        EyeImage depthPanel = fitPanel(rawDepth.view(), opts.maxPanelDim);
        res = assembleSheet({std::move(base), std::move(depthPanel)},
                            {!opts.labels.empty() ? opts.labels[0] : "scene", mode}, opts.layout, fscale);
    } else if (mode == "audit" && (opts.audit != nullptr || opts.sceneAudit != nullptr)) {
        EyeImage base = fitPanel(images[0], opts.maxPanelDim);
        const double coordScale = opts.audit ? opts.issueCoordScale : 1.0;
        const double sx = coordScale * static_cast<double>(base.width) / std::max(1, images[0].width);
        const double sy = coordScale * static_cast<double>(base.height) / std::max(1, images[0].height);
        const std::vector<EyeIssue>& issues = opts.audit ? opts.audit->issues : opts.sceneAudit->issues;
        for (size_t i = 0; i < issues.size() && i < 24; ++i) {
            const auto& iss = issues[i];
            const bool err = (iss.severity == "error");
            const uint8_t cr = err ? 240 : 245;
            const uint8_t cg = err ? 60 : 175;
            const uint8_t cb = err ? 60 : 40;
            const int rx = static_cast<int>(std::lround(iss.rect.x * sx));
            const int ry = static_cast<int>(std::lround(iss.rect.y * sy));
            const int rw = std::max(4, static_cast<int>(std::lround(iss.rect.w * sx)));
            const int rh = std::max(4, static_cast<int>(std::lround(iss.rect.h * sy)));
            drawRectBox(base, rx, ry, rw, rh, 2, cr, cg, cb);
            char tag[48];
            std::snprintf(tag, sizeof(tag), "#%zu %s", i + 1, iss.kind.c_str());
            drawText(base, std::clamp(rx, 2, std::max(2, base.width - 140)),
                     std::max(2, ry > 18 ? ry - 16 : ry + 2), tag, 1, cr, cg, cb, true);
        }
        char lbl[96];
        const int errs = opts.audit ? opts.audit->errorCount : opts.sceneAudit->errorCount;
        const int warns = opts.audit ? opts.audit->warningCount : opts.sceneAudit->warningCount;
        std::snprintf(lbl, sizeof(lbl), "audit: %d err, %d warn", errs, warns);
        res = assembleSheet({std::move(base)}, {lbl}, "side", fscale);
    } else {
        std::vector<EyeImage> panels;
        std::vector<std::string> lbls;
        for (size_t i = 0; i < images.size(); ++i) {
            if (!images[i].valid()) continue;
            panels.push_back(fitPanel(images[i], opts.maxPanelDim));
            if (i < opts.labels.size()) {
                lbls.push_back(opts.labels[i]);
            } else {
                const auto m = measureImage(images[i], {3, 3, 2, false});
                char buf[64];
                std::snprintf(buf, sizeof(buf), "#%zu L=%.2f C=%.2f", i, m.lumaMean, m.chromaMean);
                lbls.emplace_back(buf);
            }
        }
        res = assembleSheet(std::move(panels), lbls, opts.layout, fscale);
    }

    if (!opts.path.empty() && !res.rgba.empty() && res.width > 0 && res.height > 0) {
        std::error_code ec;
        const std::filesystem::path outPath(opts.path);
        if (outPath.has_parent_path()) {
            std::filesystem::create_directories(outPath.parent_path(), ec);
        }
        if (broimage::encode_png_file(opts.path, res.rgba.data(), res.width, res.height, 4)) {
            res.path = opts.path;
        }
    }
    return res;
}

}  // namespace bro::eye
