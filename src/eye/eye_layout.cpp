#include "eye/eye_layout.h"

#include "dom/document.h"
#include "dom/element.h"
#include "dom/element_geometry.h"
#include "dom/text_node.h"
#include "engine/engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace bro::eye {
namespace {

std::string toLowerCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimCopy(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string styleProp(const dom::Element* el, const char* key) {
    if (!el) return {};
    const auto& s = el->computedStyle();
    auto it = s.find(key);
    return it != s.end() ? it->second : std::string{};
}

double parseCssPx(const std::string& val, double fallback) {
    if (val.empty()) return fallback;
    char* end = nullptr;
    const double d = std::strtod(val.c_str(), &end);
    return (end != val.c_str() && std::isfinite(d)) ? d : fallback;
}

std::string buildSelector(const dom::Element* el) {
    if (!el) return "";
    std::string s = toLowerCopy(el->tagName());
    if (!el->id().empty()) s += "#" + el->id();
    const std::string cls = trimCopy(el->getAttribute("class"));
    if (!cls.empty()) {
        std::istringstream iss(cls);
        std::string tok;
        int cnt = 0;
        while (iss >> tok && cnt < 2) {
            s += "." + tok;
            ++cnt;
        }
    }
    return s;
}

std::string directText(const dom::Element* el) {
    std::string out;
    for (auto* child : el->childNodes()) {
        if (child->nodeType() == dom::NodeType::Text) {
            const std::string t = trimCopy(static_cast<const dom::TextNode*>(child)->data());
            if (!t.empty()) {
                if (!out.empty()) out += ' ';
                out += t;
            }
        }
    }
    if (out.size() > 64) out = out.substr(0, 61) + "...";
    return out;
}

struct LayoutNode {
    const dom::Element* el = nullptr;
    const dom::Element* parent = nullptr;
    EyeRect rect;
    std::string selector;
    std::string text;
    double fontSize = 14.0;
    double lineHeight = 19.0;
};

struct RailCluster {
    double sum = 0.0;
    double mean = 0.0;
    int count = 0;
};

std::vector<RailCluster> clusterRails(std::vector<double> coords, double tolPx) {
    std::vector<RailCluster> clusters;
    if (coords.empty()) return clusters;
    std::sort(coords.begin(), coords.end());
    RailCluster cur{coords[0], coords[0], 1};
    for (size_t i = 1; i < coords.size(); ++i) {
        if (coords[i] - cur.mean <= tolPx) {
            cur.sum += coords[i];
            ++cur.count;
            cur.mean = cur.sum / cur.count;
        } else {
            if (cur.count >= 2) clusters.push_back(cur);
            cur = {coords[i], coords[i], 1};
        }
    }
    if (cur.count >= 2) clusters.push_back(cur);
    return clusters;
}

}  // namespace

EyeLayoutAnalysis analyzeLayout(engine::Engine& engine, const EyeComposeOptions& opts) {
    EyeLayoutAnalysis out;
    dom::Document* doc = engine.document();
    if (!doc) return out;
    dom::Element* root = !opts.selector.empty() ? engine.querySelector(opts.selector) : doc->documentElement();
    if (!root) return out;

    out.analyzed = true;
    const float scrollY = engine.viewportScrollY();
    const int cW = std::max(1, engine.contentWidth());
    const int cH = std::max(1, engine.contentHeight());

    std::vector<LayoutNode> nodes;
    std::unordered_map<const dom::Element*, std::vector<size_t>> childrenByParent;

    auto walk = [&](auto& self, dom::Element* el, const dom::Element* parentEl) -> void {
        if (!el) return;
        const std::string& tag = el->tagName();
        if (tag == "SCRIPT" || tag == "STYLE" || tag == "HEAD" || tag == "META" ||
            tag == "LINK" || tag == "TITLE" || tag == "NOSCRIPT") {
            return;
        }
        if (styleProp(el, "display") == "none") return;
        const std::string vis = styleProp(el, "visibility");
        const dom::AbsoluteRect ab = dom::absoluteBorderBox(el);
        const EyeRect r{ab.x, ab.y - scrollY, ab.width, ab.height};

        const bool inViewport = (r.x + r.w > 0 && r.y + r.h > 0 && r.x < cW && r.y < cH);
        if (vis != "hidden" && vis != "collapse" && tag != "HTML" && tag != "BODY" &&
            r.w >= 4.0 && r.h >= 4.0 && inViewport) {
            LayoutNode ln;
            ln.el = el;
            ln.parent = parentEl;
            ln.rect = r;
            ln.selector = buildSelector(el);
            ln.text = directText(el);
            ln.fontSize = parseCssPx(styleProp(el, "font-size"), 14.0);
            const std::string lhStr = styleProp(el, "line-height");
            ln.lineHeight = (lhStr == "normal" || lhStr.empty()) ? (ln.fontSize * 1.35) : parseCssPx(lhStr, ln.fontSize * 1.35);
            const size_t idx = nodes.size();
            nodes.push_back(std::move(ln));
            if (parentEl) childrenByParent[parentEl].push_back(idx);
        }

        for (auto* ch : el->composedChildNodes()) {
            if (ch->nodeType() == dom::NodeType::Element) {
                self(self, static_cast<dom::Element*>(ch), el);
            }
        }
    };
    walk(walk, root, nullptr);

    out.elementCount = static_cast<int>(nodes.size());
    if (nodes.empty()) return out;

    // 1. Alignment Rails & Near-Miss Drift Detection
    std::vector<double> xEdges, yEdges, yCenters;
    for (const auto& n : nodes) {
        if (n.rect.w < 16.0 || n.rect.h < 14.0) continue;
        xEdges.push_back(n.rect.x);
        xEdges.push_back(n.rect.x + n.rect.w);
        yEdges.push_back(n.rect.y);
        yEdges.push_back(n.rect.y + n.rect.h);
        yCenters.push_back(n.rect.y + 0.5 * n.rect.h);
    }

    const double tol = std::clamp(opts.railTolerancePx, 0.5, 4.0);
    const double driftMax = std::clamp(opts.driftMaxPx, tol + 0.5, 16.0);
    const auto xRails = clusterRails(std::move(xEdges), tol);
    const auto yRails = clusterRails(std::move(yEdges), tol);
    const auto ycRails = clusterRails(std::move(yCenters), tol);
    out.xRailCount = static_cast<int>(xRails.size());
    out.yRailCount = static_cast<int>(yRails.size());

    for (const auto& rc : xRails) out.rails.push_back({"x", rc.mean, rc.count});
    for (const auto& rc : yRails) out.rails.push_back({"y", rc.mean, rc.count});
    std::sort(out.rails.begin(), out.rails.end(), [](const EyeRail& a, const EyeRail& b) {
        return a.count > b.count;
    });
    if (out.rails.size() > 32) out.rails.resize(32);

    out.railEconomy = std::clamp(1.0 - static_cast<double>(out.xRailCount + out.yRailCount) /
                                       std::max(4.0, static_cast<double>(out.elementCount) * 1.5),
                                 0.0, 1.0);

    auto alignedOnAny = [&](double pos, const std::vector<RailCluster>& rails) -> bool {
        for (const auto& rc : rails) {
            if (rc.count >= 2 && std::abs(pos - rc.mean) <= tol) return true;
        }
        return false;
    };

    auto findDrift = [&](const LayoutNode& n, const char* axis, double edgePos, const std::vector<RailCluster>& rails) {
        if (alignedOnAny(edgePos, rails)) return;
        const RailCluster* best = nullptr;
        double bestDist = driftMax + 1.0;
        for (const auto& rc : rails) {
            if (rc.count < 2) continue;
            const double d = std::abs(edgePos - rc.mean);
            if (d > tol && d <= driftMax && d < bestDist) {
                bestDist = d;
                best = &rc;
            }
        }
        if (best && out.drifts.size() < 24) {
            out.drifts.push_back({axis, n.selector, edgePos, best->mean, edgePos - best->mean,
                                  n.rect.x, n.rect.y, n.rect.w, n.rect.h});
        }
    };
    for (const auto& n : nodes) {
        if (n.rect.w < 16.0 || n.rect.h < 14.0) continue;
        findDrift(n, "x", n.rect.x, xRails);
        if (!alignedOnAny(n.rect.y + 0.5 * n.rect.h, ycRails)) {
            findDrift(n, "y", n.rect.y, yRails);
        }
    }

    // 2. Sibling Spacing Rhythm & Gestalt Grouping
    std::vector<double> innerGaps;
    std::vector<double> outerGaps;
    int totalGaps = 0, regularGaps = 0;

    for (const auto& kv : childrenByParent) {
        const auto& cids = kv.second;
        if (cids.size() < 3) continue;
        std::vector<const LayoutNode*> kids;
        for (size_t id : cids) {
            if (nodes[id].rect.w >= 8.0 && nodes[id].rect.h >= 8.0) kids.push_back(&nodes[id]);
        }
        if (kids.size() < 3) continue;

        // Determine dominant stack axis (row vs col)
        double spanX = 0.0, spanY = 0.0;
        double minX = 1e9, maxX = -1e9, minY = 1e9, maxY = -1e9;
        for (const auto* k : kids) {
            minX = std::min(minX, k->rect.x);
            maxX = std::max(maxX, k->rect.x + k->rect.w);
            minY = std::min(minY, k->rect.y);
            maxY = std::max(maxY, k->rect.y + k->rect.h);
        }
        spanX = maxX - minX;
        spanY = maxY - minY;
        const bool vertical = spanY >= spanX;

        std::sort(kids.begin(), kids.end(), [&](const LayoutNode* a, const LayoutNode* b) {
            return vertical ? (a->rect.y < b->rect.y) : (a->rect.x < b->rect.x);
        });

        struct GapRec { double gap; const LayoutNode* a; const LayoutNode* b; };
        std::vector<GapRec> gaps;
        for (size_t i = 0; i + 1 < kids.size(); ++i) {
            const double g = vertical
                ? (kids[i + 1]->rect.y - (kids[i]->rect.y + kids[i]->rect.h))
                : (kids[i + 1]->rect.x - (kids[i]->rect.x + kids[i]->rect.w));
            if (g >= 0.0 && g <= 120.0) {
                gaps.push_back({g, kids[i], kids[i + 1]});
                const bool isSectionGroup = kids[i]->rect.h > 80.0 || kids[i]->rect.w > 180.0;
                if (isSectionGroup) outerGaps.push_back(g);
                else innerGaps.push_back(g);
            }
        }
        if (gaps.size() < 2) continue;

        std::vector<double> vals;
        for (const auto& gr : gaps) vals.push_back(gr.gap);
        std::sort(vals.begin(), vals.end());
        const double medGap = vals[vals.size() / 2];
        const std::string pSel = buildSelector(kv.first);

        for (const auto& gr : gaps) {
            ++totalGaps;
            if (std::abs(gr.gap - medGap) <= 2.0) {
                ++regularGaps;
            } else if (out.gapOutliers.size() < 16 && std::abs(gr.gap - medGap) >= 3.0) {
                const double gx = vertical ? gr.b->rect.x : (gr.a->rect.x + gr.a->rect.w);
                const double gy = vertical ? (gr.a->rect.y + gr.a->rect.h) : gr.b->rect.y;
                const double gw = vertical ? std::max(gr.a->rect.w, gr.b->rect.w) : std::max(4.0, gr.gap);
                const double gh = vertical ? std::max(4.0, gr.gap) : std::max(gr.a->rect.h, gr.b->rect.h);
                out.gapOutliers.push_back({pSel, vertical ? "y" : "x", gr.gap, medGap, gx, gy, gw, gh});
            }
        }
    }

    if (totalGaps > 0) {
        out.spacingRegularity = static_cast<double>(regularGaps) / totalGaps;
    }
    if (!innerGaps.empty() && !outerGaps.empty()) {
        std::sort(innerGaps.begin(), innerGaps.end());
        std::sort(outerGaps.begin(), outerGaps.end());
        const double medIn = innerGaps[innerGaps.size() / 2];
        const double medOut = outerGaps[outerGaps.size() / 2];
        out.gestaltRatio = (medOut > 1.0) ? (medIn / medOut) : 1.0;
    }

    // 3. Awkward Line Wrap / Narrow Squeezed Column Text
    for (const auto& n : nodes) {
        if (n.text.size() < 16) continue;
        const int estLines = static_cast<int>(std::lround(n.rect.h / std::max(10.0, n.lineHeight)));
        if (estLines >= 3 && n.rect.w < n.fontSize * 12.5 && out.wrapIssues.size() < 16) {
            out.wrapIssues.push_back({n.selector, n.text, n.rect.w, n.rect.h, estLines,
                                      n.rect.x, n.rect.y, n.rect.w, n.rect.h});
        }
    }

    // 4. Tangent / "Kiss" Near-Touch Detection across unrelated siblings
    for (const auto& kv : childrenByParent) {
        const auto& cids = kv.second;
        if (cids.size() < 2) continue;
        // Skip flex/grid containers with explicit gap styling matching the close spacing
        const double explGap = std::max({
            parseCssPx(styleProp(kv.first, "gap"), 0.0),
            parseCssPx(styleProp(kv.first, "column-gap"), 0.0),
            parseCssPx(styleProp(kv.first, "row-gap"), 0.0),
        });
        const std::string pDisp = styleProp(kv.first, "display");
        const bool isLayoutGroup = (pDisp == "flex" || pDisp == "inline-flex" || pDisp == "grid") && explGap > 0.0;
        for (size_t i = 0; i < cids.size(); ++i) {
            for (size_t j = i + 1; j < cids.size(); ++j) {
                const auto& a = nodes[cids[i]];
                const auto& b = nodes[cids[j]];
                if (a.rect.w < 16.0 || a.rect.h < 14.0 || b.rect.w < 16.0 || b.rect.h < 14.0) continue;

                const double ox = std::min(a.rect.x + a.rect.w, b.rect.x + b.rect.w) - std::max(a.rect.x, b.rect.x);
                const double oy = std::min(a.rect.y + a.rect.h, b.rect.y + b.rect.h) - std::max(a.rect.y, b.rect.y);

                double gap = -1.0;
                if (oy >= 0.5 * std::min(a.rect.h, b.rect.h) && ox < 0.0) gap = -ox;
                else if (ox >= 0.5 * std::min(a.rect.w, b.rect.w) && oy < 0.0) gap = -oy;

                if (gap >= 0.5 && gap <= 3.0 && !isLayoutGroup && std::abs(gap - explGap) > 0.5 && out.tangents.size() < 16) {
                    const double rx = std::min(a.rect.x, b.rect.x);
                    const double ry = std::min(a.rect.y, b.rect.y);
                    const double rx1 = std::max(a.rect.x + a.rect.w, b.rect.x + b.rect.w);
                    const double ry1 = std::max(a.rect.y + a.rect.h, b.rect.y + b.rect.h);
                    out.tangents.push_back({a.selector, b.selector, gap, rx, ry, rx1 - rx, ry1 - ry});
                }
            }
        }
    }

    return out;
}

}  // namespace bro::eye
