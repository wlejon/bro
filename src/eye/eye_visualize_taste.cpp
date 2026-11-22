#include "eye/eye_visualize_taste.h"
#include "eye/eye_color.h"
#include "eye/eye_compose.h"
#include "eye/eye_draw_internal.h"
#include "eye/eye_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace bro::eye {
namespace {

bool unprojectDepthPos(const EyeVisualizeOptions& opts, int x, int y, int dw, int dh, float d, double out[3]) {
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
}

bool reconstructViewNormal(const EyeVisualizeOptions& opts, int x, int y, int dw, int dh,
                           double outN[3], double outPos[3]) {
    const size_t idx = static_cast<size_t>(y) * dw + x;
    if (!unprojectDepthPos(opts, x, y, dw, dh, opts.depth[idx], outPos)) return false;

    double pr[3], pu[3];
    const bool hasR = (x + 1 < dw) && unprojectDepthPos(opts, x + 1, y, dw, dh, opts.depth[idx + 1], pr);
    const bool hasL = !hasR && (x > 0) && unprojectDepthPos(opts, x - 1, y, dw, dh, opts.depth[idx - 1], pr);
    const bool hasU = (y > 0) && unprojectDepthPos(opts, x, y - 1, dw, dh, opts.depth[idx - dw], pu);
    const bool hasD = !hasU && (y + 1 < dh) && unprojectDepthPos(opts, x, y + 1, dw, dh, opts.depth[idx + dw], pu);
    if ((!hasR && !hasL) || (!hasU && !hasD)) return false;

    double dpx[3] = {
        hasR ? (pr[0] - outPos[0]) : (outPos[0] - pr[0]),
        hasR ? (pr[1] - outPos[1]) : (outPos[1] - pr[1]),
        hasR ? (pr[2] - outPos[2]) : (outPos[2] - pr[2]),
    };
    double dpy[3] = {
        hasU ? (pu[0] - outPos[0]) : (outPos[0] - pu[0]),
        hasU ? (pu[1] - outPos[1]) : (outPos[1] - pu[1]),
        hasU ? (pu[2] - outPos[2]) : (outPos[2] - pu[2]),
    };
    double nx = dpx[1] * dpy[2] - dpx[2] * dpy[1];
    double ny = dpx[2] * dpy[0] - dpx[0] * dpy[2];
    double nz = dpx[0] * dpy[1] - dpx[1] * dpy[0];
    if (nz < 0.0) { nx = -nx; ny = -ny; nz = -nz; }
    const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (len < 1e-12) return false;
    outN[0] = nx / len;
    outN[1] = ny / len;
    outN[2] = nz / len;
    return true;
}

}  // namespace

bool visualizeTasteMode(const std::string& mode,
                        const std::vector<EyeImageView>& images,
                        const EyeVisualizeOptions& opts,
                        EyeVisualResult& outResult) {
    if (images.empty() || !images[0].valid()) return false;
    const int fscale = std::clamp(opts.fontScale, 1, 4);

    if (mode == "motion" && images.size() >= 2) {
        EyeMotionOptions mopts;
        mopts.labels = opts.labels;
        mopts.maxPanelDim = opts.maxPanelDim;
        mopts.layout = opts.layout;
        mopts.buildVisual = true;
        const EyeMotionResult mr = analyzeMotion(images, mopts);
        if (!mr.visual.empty()) {
            outResult.width = mr.visual.width;
            outResult.height = mr.visual.height;
            outResult.rgba = mr.visual.rgba;
            return true;
        }
        return false;
    }

    if (mode == "saliency") {
        EyeImage base = draw::fitPanel(images[0], opts.maxPanelDim);
        const EyeComposeResult comp = opts.compose ? *opts.compose : analyzeComposition(images[0], {});
        int sw = 0, sh = 0;
        std::vector<float> sal;
        computeSaliencyField(images[0], sw, sh, sal);
        EyeImage heat{base.width, base.height, std::vector<uint8_t>(static_cast<size_t>(base.width) * base.height * 4)};
        for (int y = 0; y < base.height; ++y) {
            const int sy = (sw > 0 && sh > 0) ? std::clamp((y * sh) / base.height, 0, sh - 1) : 0;
            for (int x = 0; x < base.width; ++x) {
                const int sx = (sw > 0 && sh > 0) ? std::clamp((x * sw) / base.width, 0, sw - 1) : 0;
                const float v = (sw > 0 && sh > 0) ? sal[static_cast<size_t>(sy) * sw + sx] : 0.0f;
                const auto c = color::magmaColor(v);
                const size_t i = (static_cast<size_t>(y) * base.width + x) * 4;
                heat.rgba[i + 0] = c.r; heat.rgba[i + 1] = c.g; heat.rgba[i + 2] = c.b; heat.rgba[i + 3] = 255;
            }
        }

        // Draw scanpath polyline & numbered anchors on base frame + heatmap
        int prevX = -1, prevY = -1;
        for (const auto& fp : comp.saliency.anchors) {
            const int ax = static_cast<int>(std::lround(fp.x * base.width));
            const int ay = static_cast<int>(std::lround(fp.y * base.height));
            const int rad = std::clamp(static_cast<int>(std::lround(fp.radius * std::min(base.width, base.height) * 0.45)), 8, 28);
            if (prevX >= 0) {
                draw::drawLine(base, prevX, prevY, ax, ay, 2, 70, 220, 255, 210);
            }
            const bool isTop = (fp.rank == 1);
            const uint8_t cr = isTop ? 255 : 70;
            const uint8_t cg = isTop ? 215 : 220;
            const uint8_t cb = isTop ? 40 : 255;
            draw::drawCircle(base, ax, ay, rad, 2, cr, cg, cb, 235);
            char tag[16];
            std::snprintf(tag, sizeof(tag), "#%d", fp.rank);
            draw::drawText(base, std::clamp(ax + 4, 2, std::max(2, base.width - 24)),
                           std::clamp(ay - 12, 2, std::max(2, base.height - 14)), tag, 1, cr, cg, cb, true);
            prevX = ax; prevY = ay;
        }
        // Visual Center of Mass crosshair in green
        const int comX = static_cast<int>(std::lround(comp.saliency.centerOfMassX * base.width));
        const int comY = static_cast<int>(std::lround(comp.saliency.centerOfMassY * base.height));
        draw::drawLine(base, comX - 8, comY, comX + 8, comY, 2, 80, 240, 130);
        draw::drawLine(base, comX, comY - 8, comX, comY + 8, 2, 80, 240, 130);

        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "saliency | dom %.2f | bal (%+.2f,%+.2f)",
                      comp.saliency.focalDominance, comp.saliency.balanceX, comp.saliency.balanceY);
        outResult = draw::assembleSheet({std::move(base), std::move(heat)},
                                        {!opts.labels.empty() ? opts.labels[0] : "scanpath", lbl},
                                        opts.layout, fscale);
        return true;
    }

    if (mode == "notan") {
        EyeImage base = draw::fitPanel(images[0], opts.maxPanelDim);
        const int nTones = (opts.notanTones >= 5) ? 5 : 3;
        int nw = 0, nh = 0;
        std::vector<uint8_t> bands;
        computeNotanField(images[0], nTones, nw, nh, bands);
        EyeImage notan{base.width, base.height, std::vector<uint8_t>(static_cast<size_t>(base.width) * base.height * 4)};
        for (int y = 0; y < base.height; ++y) {
            const int ny = (nw > 0 && nh > 0) ? std::clamp((y * nh) / base.height, 0, nh - 1) : 0;
            for (int x = 0; x < base.width; ++x) {
                const int nx = (nw > 0 && nh > 0) ? std::clamp((x * nw) / base.width, 0, nw - 1) : 0;
                const uint8_t b = (nw > 0 && nh > 0) ? bands[static_cast<size_t>(ny) * nw + nx] : 0;
                const uint8_t lum = (nTones == 3)
                    ? (b == 0 ? 18 : (b == 1 ? 118 : 238))
                    : static_cast<uint8_t>(18 + std::min<int>(4, b) * 55);
                const size_t i = (static_cast<size_t>(y) * base.width + x) * 4;
                notan.rgba[i + 0] = lum; notan.rgba[i + 1] = lum; notan.rgba[i + 2] = lum; notan.rgba[i + 3] = 255;
            }
        }
        EyeComposeOptions co;
        co.notanTones = nTones;
        const EyeComposeResult comp = opts.compose ? *opts.compose : analyzeComposition(images[0], co);
        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "notan %dt | masses %d | D/M/L %.0f/%.0f/%.0f",
                      nTones, comp.notan.massCount,
                      comp.notan.darkShare * 100.0, comp.notan.midShare * 100.0, comp.notan.lightShare * 100.0);
        outResult = draw::assembleSheet({std::move(base), std::move(notan)},
                                        {!opts.labels.empty() ? opts.labels[0] : "frame", lbl},
                                        opts.layout, fscale);
        return true;
    }

    if (mode == "rails" && opts.compose && opts.compose->layout.analyzed) {
        EyeImage base = draw::fitPanel(images[0], opts.maxPanelDim);
        const double scale = opts.issueCoordScale * static_cast<double>(base.width) / std::max(1, images[0].width);
        const auto& lay = opts.compose->layout;

        for (size_t i = 0; i < lay.rails.size() && i < 24; ++i) {
            const auto& r = lay.rails[i];
            const int p = static_cast<int>(std::lround(r.pos * scale));
            if (r.axis == "x") draw::drawLine(base, p, 0, p, base.height - 1, 1, 55, 200, 235, 110);
            else draw::drawLine(base, 0, p, base.width - 1, p, 1, 55, 200, 235, 110);
        }
        for (size_t i = 0; i < lay.drifts.size() && i < 16; ++i) {
            const auto& d = lay.drifts[i];
            const int rx = static_cast<int>(std::lround(d.rectX * scale));
            const int ry = static_cast<int>(std::lround(d.rectY * scale));
            const int rw = std::max(4, static_cast<int>(std::lround(d.rectW * scale)));
            const int rh = std::max(4, static_cast<int>(std::lround(d.rectH * scale)));
            draw::drawRectBox(base, rx, ry, rw, rh, 2, 245, 70, 70);
            char tag[32];
            std::snprintf(tag, sizeof(tag), "%s %+.1fpx", d.axis.c_str(), d.driftPx);
            draw::drawText(base, std::clamp(rx, 2, std::max(2, base.width - 70)), std::max(2, ry - 12), tag, 1, 245, 90, 90, true);
        }
        for (size_t i = 0; i < lay.wrapIssues.size() && i < 12; ++i) {
            const auto& w = lay.wrapIssues[i];
            draw::drawRectBox(base, static_cast<int>(std::lround(w.rectX * scale)),
                              static_cast<int>(std::lround(w.rectY * scale)),
                              std::max(4, static_cast<int>(std::lround(w.rectW * scale))),
                              std::max(4, static_cast<int>(std::lround(w.rectH * scale))), 2, 245, 195, 50);
        }
        for (size_t i = 0; i < lay.tangents.size() && i < 12; ++i) {
            const auto& t = lay.tangents[i];
            draw::drawRectBox(base, static_cast<int>(std::lround(t.rectX * scale)),
                              static_cast<int>(std::lround(t.rectY * scale)),
                              std::max(4, static_cast<int>(std::lround(t.rectW * scale))),
                              std::max(4, static_cast<int>(std::lround(t.rectH * scale))), 2, 235, 90, 235);
        }

        char lbl[96];
        std::snprintf(lbl, sizeof(lbl), "rails %d | drifts %zu | wrap %zu | kiss %zu",
                      lay.xRailCount + lay.yRailCount, lay.drifts.size(), lay.wrapIssues.size(), lay.tangents.size());
        outResult = draw::assembleSheet({std::move(base)}, {lbl}, "side", fscale);
        return true;
    }

    if ((mode == "zebra" || mode == "curvature") && opts.depth && opts.depthWidth > 0 && opts.depthHeight > 0) {
        const int dw = opts.depthWidth, dh = opts.depthHeight;
        EyeImage diag{dw, dh, std::vector<uint8_t>(static_cast<size_t>(dw) * dh * 4, 255)};
        std::vector<double> nBuf(static_cast<size_t>(dw) * dh * 3, 0.0);
        std::vector<uint8_t> valid(static_cast<size_t>(dw) * dh, 0);

        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                const size_t idx = static_cast<size_t>(y) * dw + x;
                double n[3], p[3];
                if (reconstructViewNormal(opts, x, y, dw, dh, n, p)) {
                    valid[idx] = 1;
                    nBuf[idx * 3 + 0] = n[0];
                    nBuf[idx * 3 + 1] = n[1];
                    nBuf[idx * 3 + 2] = n[2];
                }
            }
        }

        const double freq = std::clamp(opts.zebraFreq, 2.0, 48.0);
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                const size_t idx = static_cast<size_t>(y) * dw + x;
                if (!valid[idx]) {
                    diag.rgba[idx * 4 + 0] = 18; diag.rgba[idx * 4 + 1] = 20; diag.rgba[idx * 4 + 2] = 26;
                    continue;
                }
                if (mode == "zebra") {
                    // View vector from surface toward camera: in view space camera is origin, so V = -normalize(P)
                    double p[3];
                    unprojectDepthPos(opts, x, y, dw, dh, opts.depth[idx], p);
                    const double plen = std::max(1e-6, std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
                    const double vx = -p[0] / plen, vy = -p[1] / plen, vz = -p[2] / plen;
                    const double nx = nBuf[idx * 3 + 0], ny = nBuf[idx * 3 + 1], nz = nBuf[idx * 3 + 2];
                    const double vn = vx * nx + vy * ny + vz * nz;
                    const double rx = 2.0 * vn * nx - vx;
                    const double ry = 2.0 * vn * ny - vy;
                    const double proj = ry * 0.85 + rx * 0.52;
                    const double wave = std::sin(proj * freq * 3.14159265);
                    const double smoothBand = std::clamp(0.5 + 3.0 * wave, 0.0, 1.0);
                    const uint8_t c = static_cast<uint8_t>(24 + smoothBand * 224);
                    diag.rgba[idx * 4 + 0] = c;
                    diag.rgba[idx * 4 + 1] = c;
                    diag.rgba[idx * 4 + 2] = static_cast<uint8_t>(std::min(255, int(c) + (wave > 0 ? 4 : 0)));
                } else {
                    // Mean curvature estimate from screen-space normal divergence (-div N)
                    double dnx = 0.0, dny = 0.0;
                    if (x > 1 && x + 1 < dw && valid[idx - 1] && valid[idx + 1]) {
                        dnx = 0.5 * (nBuf[(idx + 1) * 3 + 0] - nBuf[(idx - 1) * 3 + 0]);
                    }
                    if (y > 1 && y + 1 < dh && valid[idx - dw] && valid[idx + dw]) {
                        dny = 0.5 * (nBuf[(idx + dw) * 3 + 1] - nBuf[(idx - dw) * 3 + 1]);
                    }
                    const double meanK = std::clamp(-(dnx - dny) * 6.5, -1.0, 1.0);
                    // Cool cyan-blue for concave troughs (<0), neutral dark gray at 0, warm gold-red for convex crests (>0)
                    uint8_t r = 110, g = 115, b = 122;
                    if (meanK > 0.0) {
                        r = static_cast<uint8_t>(110 + meanK * 140);
                        g = static_cast<uint8_t>(115 + meanK * 75);
                        b = static_cast<uint8_t>(122 - meanK * 92);
                    } else {
                        const double t = -meanK;
                        r = static_cast<uint8_t>(110 - t * 85);
                        g = static_cast<uint8_t>(115 + t * 50);
                        b = static_cast<uint8_t>(122 + t * 128);
                    }
                    diag.rgba[idx * 4 + 0] = r; diag.rgba[idx * 4 + 1] = g; diag.rgba[idx * 4 + 2] = b;
                }
            }
        }

        EyeImage base = draw::fitPanel(images[0], opts.maxPanelDim);
        EyeImage diagPanel = draw::fitPanel(diag.view(), opts.maxPanelDim);
        outResult = draw::assembleSheet({std::move(base), std::move(diagPanel)},
                                        {!opts.labels.empty() ? opts.labels[0] : "scene", mode},
                                        opts.layout, fscale);
        return true;
    }

    return false;
}

}  // namespace bro::eye
