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
    check(!g.volumesCurrent(0), "a new cascade's volumes are a job");
    check(!g.traceJobs().empty(), "its probes trace the frame its volumes are made (their job records first)");
    makeAll(g);
    bool overlap = false;
    check(freshColumns(g, 0, overlap).size() == 64 && !overlap, "a new square is fresh, every column once");
    int required = 0, widest = 0;
    for (const auto& j : g.traceJobs()) {
        required += j.required ? 1 : 0;
        if (j.box.fresh) widest = std::max(widest, j.box.columnCount());
    }
    // Eight boxes a cascade, a row each; the outermost rows of the two finer
    // cascades are unseen, so wanted, not required.
    check(required == 20 && widest == 8, "fresh boxes are required where seen, freshColumns columns each");
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
    std::vector<ProbeBox> stale;
    g.staleBoxes(0, stale);
    check(stale.size() == 2 && stale[0].x0 == 6 && stale[0].x1 == 8 && stale[0].z0 == 0 && stale[0].z1 == 8 &&
              stale[1].x0 == 0 && stale[1].x1 == 6 && stale[1].z0 == 0 && stale[1].z1 == 1,
          "its stale world is the move's strips");
    g.volumesMade(0);
    bool overlap = false;
    const auto cols = freshColumns(g, 0, overlap);
    // Two new columns (x 6, 7: 16 cells), one new row (z 0) less theirs: 6.
    check(cols.size() == 22 && !overlap, "the move's strips: every new column once");
    bool onlyNew = true;
    for (auto [x, z] : cols) onlyNew = onlyNew && (x >= 6 || z == 0);
    check(onlyNew, "the strips hold only the new columns");

    // Moved again before tracing: the queued strips move with the square.
    g.place(6.0, -2.0);   // one more column east
    g.staleBoxes(0, stale);
    check(stale.size() == 1 && stale[0].x0 == 7 && stale[0].x1 == 8 && stale[0].z0 == 0 && stale[0].z1 == 8,
          "a made world's stale strip is the new column alone");
    g.volumesMade(0);
    const auto again = freshColumns(g, 0, overlap);
    // The old x 6, 7 are now x 5, 6; the new x 7; the old row z 0 shifted
    // west a column (x 0..4 of it now under x < 5 kept): 8 + 8 + 8 + 5.
    bool covered = again.count({7, 3}) && again.count({5, 3}) && again.count({6, 3}) && again.count({0, 0});
    check(covered && !overlap, "queued strips move with the square, the new column added");
    check(!again.count({0, 3}), "an old column stays traced");

    // Moved a column every frame (the world remade each frame): each
    // frame's strip is offered the frame after its world, never piled up.
    g.volumesMade(0);
    traceFresh(g);
    bool eachFrame = true;
    for (int f = 1; f <= 10; ++f) {
        g.place(6.0 + 2.0 * f, -2.0);
        eachFrame = eachFrame && !g.volumesCurrent(0) && freshColumns(g, 0, overlap).empty();
        g.volumesMade(0);
        eachFrame = eachFrame && freshColumns(g, 0, overlap).size() == 8;
        traceFresh(g);
    }
    check(eachFrame, "a cascade moving every frame traces each frame's strip once its world is made");

    // A jump past the square: all of it fresh.
    g.place(1000.0, 1000.0);
    g.staleBoxes(0, stale);
    check(stale.size() == 1 && stale[0].columnCount() == 64, "a jump makes the whole world stale");
    g.volumesMade(0);
    check(freshColumns(g, 0, overlap).size() == 64, "a jump makes the whole square fresh");
}

// The strip a one-column move brings in is the square's outermost column,
// which no lit surface sees: wanted, not required, until the next move
// takes it inside; the last cascade's is seen.
void testUnseen() {
    ProbeGrid g = grid(8);
    g.place(0.0, 0.0);
    makeAll(g);
    traceFresh(g);
    g.place(2.0, 0.0);   // one column east in cascade 0 alone
    check(!g.volumesCurrent(0) && !g.staleSeen(0), "a one-column move's world is stale and unseen");
    bool overlap = false;
    check(freshColumns(g, 0, overlap).empty(), "its fresh traces wait for its world");
    g.volumesMade(0);
    bool wanted = true;
    int boxes = 0;
    for (const auto& j : g.traceJobs()) {
        if (!j.box.fresh) continue;
        ++boxes;
        wanted = wanted && !j.required && j.error == g.config().unseenPixels && j.box.x0 == 7 && j.box.columnCount() <= 8;
    }
    check(boxes == 1 && wanted, "then they are offered, wanted at unseenPixels, not required");
    // Not traced, and moved on: the strip is inside now, and required; the
    // new one is unseen.
    g.place(4.0, 0.0);
    check(!g.staleSeen(0), "the next strip's world is unseen too");
    bool inside = false, early = false;
    for (const auto& j : g.traceJobs()) {
        if (!j.box.fresh) continue;
        inside = inside || (j.box.x0 == 6 && j.required);
        early = early || j.box.x0 == 7;
    }
    check(inside && !early, "a strip taken inside is required; the new one waits for its world");
    // Two columns at once: the strip is two wide, and seen.
    g.volumesMade(0);
    traceFresh(g);
    g.place(8.0, 0.0);
    check(g.staleSeen(0), "a two-column move's world is seen (required)");
    // The last cascade blends into nothing: its edge is seen.
    g.place(36.0, 0.0);
    check(g.staleSeen(2), "the last cascade's outermost column is seen");
}

void testVolumes() {
    ProbeGrid g = grid(8);
    g.place(0.0, 0.0);
    makeAll(g);
    check(g.volumesCurrent(0) && g.volumesCurrent(2), "made volumes are current");
    g.canopyChanged();
    check(!g.volumesCurrent(0) && !g.volumesCurrent(2), "a canopy change everywhere remakes every cascade's");
    makeAll(g);
    // Cascade 0 spans -8..8 m (8 columns of 2 m), cascade 1 -32..32,
    // cascade 2 -128..128: a change at 50 m is the outer one's alone.
    g.canopyChanged(48.0, 48.0, 52.0, 52.0);
    check(g.volumesCurrent(0) && g.volumesCurrent(1) && !g.volumesCurrent(2),
          "a canopy change in a box remakes the cascades it overlaps");
    // Cascade 2's corner is column -4, and column k's world is 32 m from
    // (k - 4.5) * 32: 48..52 m lies in column 6's, [48, 80).
    std::vector<ProbeBox> stale;
    g.staleBoxes(2, stale);
    check(stale.size() == 1 && stale[0].x0 == 6 && stale[0].x1 == 7 && stale[0].z0 == 6 && stale[0].z1 == 7,
          "only the columns over the change are stale");
    g.canopyChanged(70.0, 48.0, 90.0, 52.0);   // columns 6 and 7
    g.staleBoxes(2, stale);
    check(stale.size() == 1 && stale[0].x0 == 6 && stale[0].x1 == 8 && stale[0].z0 == 6 && stale[0].z1 == 7,
          "stale boxes that overlap are merged (a voxel is splatted once)");
    makeAll(g);
    // A sun low in the east (toward +x, 0.8 up): the crowns' shade reaches
    // 32 / 0.8 * 0.6 = 24 m west of them, into column 5.
    g.setSun(0.6f, 0.8f, 0.0f, 1);
    g.canopyChanged(48.0, 48.0, 52.0, 52.0);
    g.staleBoxes(2, stale);
    check(stale.size() == 1 && stale[0].x0 == 5 && stale[0].x1 == 7 && stale[0].z0 == 6 && stale[0].z1 == 7,
          "and those its shade falls on, away from the sun");
    makeAll(g);
    g.canopyChanged(300.0, 300.0, 310.0, 310.0);
    check(g.volumesCurrent(2), "one past every cascade remakes none");
    g.setSun(0.6f, 0.8f, 0.0f, 9);
    check(!g.volumesCurrent(1), "a sun turn remakes them");
    g.staleBoxes(1, stale);
    check(stale.size() == 1 && stale[0].columnCount() == 64, "all of them");
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
    testUnseen();
    testVolumes();
    testRefresh();
    std::printf("probe grid: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
