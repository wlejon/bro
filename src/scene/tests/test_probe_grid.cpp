// ProbeGrid (scene/world/probe_grid.h), the probe grid's CPU half: the
// layers' heights and their inverse, the cascades' corners snapped about
// the eye, the columns a move brings in (fresh, required, every new column
// once and no old one), boxes still queued moving with the square, the
// volumes remade on a move, a canopy change or a sun turn, and the refresh
// slices' errors growing with their age.

#include "scene/world/probe_grid.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <utility>

using namespace bro::scene;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

ProbeGrid grid(int columns = 8) {
    ProbeGridConfig c;
    c.cascades = 3;
    c.columns = columns;
    c.layers = 8;
    c.spacing = 2.0f;
    c.ratio = 4.0f;
    c.sliceRows = 2;
    c.refreshPixelsPerSecond = 1.0f;
    ProbeGrid g;
    g.configure(c);
    return g;
}

void makeAll(ProbeGrid& g) {
    for (int c = 0; c < g.config().cascades; ++c) g.volumesMade(c);
}

// The fresh boxes' columns of cascade c in this frame's jobs, each once.
std::set<std::pair<int, int>> freshColumns(ProbeGrid& g, int c, bool& overlap) {
    std::set<std::pair<int, int>> cols;
    overlap = false;
    for (const auto& j : g.traceJobs()) {
        if (!j.box.fresh || j.box.cascade != c) continue;
        for (int x = j.box.x0; x < j.box.x1; ++x)
            for (int z = j.box.z0; z < j.box.z1; ++z) overlap = !cols.insert({x, z}).second || overlap;
    }
    return cols;
}

void traceFresh(ProbeGrid& g) {
    const auto& jobs = g.traceJobs();
    for (int i = static_cast<int>(jobs.size()) - 1; i >= 0; --i)
        if (jobs[static_cast<size_t>(i)].box.fresh) g.traced(i);
}

void testLayers() {
    ProbeGrid g = grid();
    g.setTop(32.0f);
    check(std::abs(g.layerHeight(0) - ProbeGrid::kBottom) < 1e-5f, "the lowest layer just over the ground");
    check(std::abs(g.layerHeight(7) - 32.0f) < 1e-4f, "the top layer at the top");
    bool rising = true, widening = true;
    for (int k = 1; k < 8; ++k) {
        rising = rising && g.layerHeight(k) > g.layerHeight(k - 1);
        if (k > 1) widening = widening && g.layerHeight(k) - g.layerHeight(k - 1) > g.layerHeight(k - 1) - g.layerHeight(k - 2);
        check(std::abs(g.layerAt(g.layerHeight(k)) - static_cast<float>(k)) < 1e-3f, "layerAt inverts layer " + std::to_string(k));
    }
    check(rising && widening, "the layers rise, further apart upward");
    check(g.layerAt(-5.0f) == 0.0f && g.layerAt(500.0f) == 7.0f, "layerAt clamps to the layers");
}

void testPlace() {
    ProbeGrid g = grid(8);
    g.place(5.0, -3.0);
    // Cascade 0: 2 m columns, the eye's column round(5/2) = 3 and
    // round(-3/2) = -1 (halves round up), the corner 4 back.
    check(g.cornerX(0) == -1 && g.cornerZ(0) == -5, "cascade 0's corner about the eye");
    // Cascade 2: 32 m columns, the eye's column 0.
    check(g.cornerX(2) == -4 && g.cornerZ(2) == -4, "cascade 2's corner about the eye");
    check(std::abs(g.spacing(2) - 32.0) < 1e-9, "spacing by the ratio");
    check(g.traceJobs().empty(), "nothing traces before its volumes are made");
    check(!g.volumesCurrent(0), "a new cascade's volumes are a job");
    makeAll(g);
    bool overlap = false;
    check(freshColumns(g, 0, overlap).size() == 64 && !overlap, "a new square is fresh, every column once");
    int required = 0, deepest = 0;
    for (const auto& j : g.traceJobs()) {
        required += j.required ? 1 : 0;
        if (j.box.fresh) deepest = std::max(deepest, j.box.z1 - j.box.z0);
    }
    check(required == 12 && deepest == 2, "fresh boxes are required, a slice's rows each (four a cascade)");
    traceFresh(g);
    check(freshColumns(g, 0, overlap).empty(), "traced, the fresh boxes go");
}

void testMoves() {
    ProbeGrid g = grid(8);
    g.place(0.0, 0.0);
    makeAll(g);
    traceFresh(g);
    // Two columns east and one north in cascade 0 (2 m columns).
    g.place(4.0, -2.0);
    check(g.cornerX(0) == -2 && g.cornerZ(0) == -5, "the corner moves in whole columns");
    check(!g.volumesCurrent(0) && g.volumesCurrent(2), "the moved cascade's volumes are a job, the still one's not");
    bool overlap = false;
    check(freshColumns(g, 0, overlap).empty(), "its fresh boxes wait for its volumes");
    g.volumesMade(0);
    const auto cols = freshColumns(g, 0, overlap);
    // Two new columns (x 6, 7: 16 cells), one new row (z 0) less theirs: 6.
    check(cols.size() == 22 && !overlap, "the move's strips: every new column once");
    bool onlyNew = true;
    for (auto [x, z] : cols) onlyNew = onlyNew && (x >= 6 || z == 0);
    check(onlyNew, "the strips hold only the new columns");

    // Moved again before tracing: the queued strips move with the square.
    g.place(6.0, -2.0);   // one more column east
    g.volumesMade(0);
    const auto again = freshColumns(g, 0, overlap);
    // The old x 6, 7 are now x 5, 6; the new x 7; the old row z 0 shifted
    // west a column (x 0..4 of it now under x < 5 kept): 8 + 8 + 8 + 5.
    bool covered = again.count({7, 3}) && again.count({5, 3}) && again.count({6, 3}) && again.count({0, 0});
    check(covered && !overlap, "queued strips move with the square, the new column added");
    check(!again.count({0, 3}), "an old column stays traced");

    // A jump past the square: all of it fresh.
    g.place(1000.0, 1000.0);
    g.volumesMade(0);
    check(freshColumns(g, 0, overlap).size() == 64, "a jump makes the whole square fresh");
}

void testVolumes() {
    ProbeGrid g = grid(8);
    g.place(0.0, 0.0);
    makeAll(g);
    check(g.volumesCurrent(0) && g.volumesCurrent(2), "made volumes are current");
    g.setCanopyGeneration(5);
    check(!g.volumesCurrent(0) && !g.volumesCurrent(2), "a canopy change remakes every cascade's");
    makeAll(g);
    g.setSunGeneration(9);
    check(!g.volumesCurrent(1), "a sun turn remakes them");
    makeAll(g);
    g.setTop(40.0f);
    check(!g.volumesCurrent(1), "a new top remakes them");
}

void testRefresh() {
    ProbeGrid g = grid(8);
    g.place(0.0, 0.0);
    makeAll(g);
    traceFresh(g);
    g.advance(2.0f);
    // The slices start spread over their period (a second in cascade 0, two
    // in cascade 1): two seconds on, cascade 0's are 2, 1.75, 1.5 and 1.25 s
    // old, all due; cascade 1's 2, 1.5, 1 and 0.5, the first due.
    float e0 = -1.0f, e1 = -1.0f;
    int slices0 = 0, slices1 = 0;
    for (const auto& j : g.traceJobs()) {
        if (j.box.fresh) continue;
        if (j.box.cascade == 0) {
            if (j.box.z0 == 0) e0 = j.error;
            ++slices0;
            check(j.box.x0 == 0 && j.box.x1 == 8 && j.box.z1 - j.box.z0 == 2 && !j.required, "a slice is whole rows, not required");
        }
        if (j.box.cascade == 1) {
            e1 = j.error;
            ++slices1;
        }
    }
    check(slices0 == 4 && slices1 == 1, "a cascade's rows in slices, due one by one over the period");
    check(std::abs(e0 - 2.0f) < 1e-5f && std::abs(e1 - 1.0f) < 1e-5f, "a slice's error: its age, halved a cascade");
    const auto& jobs = g.traceJobs();
    for (int i = 0; i < static_cast<int>(jobs.size()); ++i)
        if (jobs[static_cast<size_t>(i)].box.cascade == 0 && jobs[static_cast<size_t>(i)].box.z0 == 2) g.traced(i);
    float traced = -1.0f, other = -1.0f;
    for (const auto& j : g.traceJobs()) {
        if (j.box.cascade != 0) continue;
        if (j.box.z0 == 2) traced = j.error;
        if (j.box.z0 == 4) other = j.error;
    }
    check(traced < 0.0f && other > 1.4f, "a traced slice waits its period again, the others do not");
    g.advance(0.5f);
    bool early = false;
    for (const auto& j : g.traceJobs()) early = early || (j.box.cascade == 0 && j.box.z0 == 2);
    check(!early, "half a period on, it still waits");
    g.advance(0.5f);
    bool due = false;
    for (const auto& j : g.traceJobs()) due = due || (j.box.cascade == 0 && j.box.z0 == 2);
    check(due, "a period on, it is offered again");
}

}  // namespace

int main() {
    testLayers();
    testPlace();
    testMoves();
    testVolumes();
    testRefresh();
    std::printf("probe grid: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
