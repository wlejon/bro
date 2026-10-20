// WorldService (scene/world/world_service.h), the world service's policy:
// one queue of jobs across producers ranked by their error in pixels, ties
// to the larger tile, granted from a budget in milliseconds that follows the
// frame's measured slack, with a floor that a frame over its target takes it
// to (and a cut in the view, until the new view is measured), and each
// kind's cost learned from its measured spans; and the executor's test for a
// cut.

#include "scene/world/world_service.h"
#include "scene/vulkan/world/scene_world_service.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace bro::scene;

namespace {

int g_checks = 0, g_failures = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (ok) return;
    if (++g_failures <= 40) std::fprintf(stderr, "FAIL: %s\n", what.c_str());
}

bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

/// A frame the timer read: `frameMs` in all, kind `kind`'s `jobs` taking `ms`.
WorldFrameTiming frameOf(double frameMs, int kind = 0, uint32_t jobs = 0, double ms = 0.0, bool apart = false) {
    WorldFrameTiming t;
    t.frameMs = frameMs;
    if (jobs > 0) t.spans.push_back({kind, jobs, ms, apart});
    return t;
}

void testRankingAcrossProducers() {
    WorldService s;
    WorldServiceConfig c;
    c.floorMs = 0.375f;   // (costs and budgets in binary fractions: exact sums)
    s.configure(c);
    const int turf = s.addKind({"turf", WorldSchedule::Raster, 0.125f});
    const int cards = s.addKind({"cards", WorldSchedule::Compute, 0.125f});
    check(turf == 0 && cards == 1, "kinds are numbered as added");
    s.beginFrame();
    check(near(s.budgetMs(), 0.375), "before any frame is read the budget is the floor");
    // Errors interleaved between the two producers: what is most wrong goes
    // first, whoever offers it.
    s.offer({1, 0, turf, 2.0f, 1.0, false});
    s.offer({2, 0, cards, 9.0f, 1.0, false});
    s.offer({3, 0, turf, 7.0f, 1.0, false});
    s.offer({4, 0, cards, 1.0f, 1.0, false});
    s.offer({5, 0, turf, 0.5f, 1.0, false});
    s.schedule();
    const auto t = s.granted(turf), k = s.granted(cards);
    check(k.size() == 1 && k[0].key == 2, "the worst job (another producer's) is granted");
    check(t.size() == 2 && t[0].key == 3 && t[1].key == 1, "then the next worst, across producers, in rank order");
    check(s.stats().granted == 3 && s.stats().queued == 2, "three fit 0.375 ms at 0.125 each; two are left");
    check(near(s.stats().worstError, 1.0f), "the worst error left is the largest of those left");
    check(near(s.stats().spentMs, 0.375), "spent is the granted jobs' cost");

    // A required job goes whatever the budget, ahead of everything.
    s.beginFrame();
    s.offer({10, 0, turf, 50.0f, 1.0, false});
    s.offer({11, 0, turf, 0.1f, 64.0, true});
    s.offer({12, 0, turf, 0.2f, 64.0, true});
    s.offer({13, 0, turf, 0.3f, 64.0, true});
    s.offer({14, 0, turf, 0.4f, 64.0, true});
    s.schedule();
    check(s.granted(turf).size() == 4, "required jobs go whatever the budget, and leave none for the rest");
    check(s.stats().queued == 1 && near(s.stats().worstError, 50.0f), "the unrequired one waits");

    // A kind that does not fit leaves room for a cheaper one.
    WorldService m;
    m.configure(c);
    const int big = m.addKind({"big", WorldSchedule::Raster, 0.25f});
    const int small = m.addKind({"small", WorldSchedule::Raster, 0.02f});
    m.beginFrame();
    m.offer({1, 0, big, 9.0f, 1.0, false});
    m.offer({2, 0, big, 8.0f, 1.0, false});
    m.offer({3, 0, small, 1.0f, 1.0, false});
    m.offer({4, 0, small, 0.5f, 1.0, false});
    m.schedule();
    check(m.granted(big).size() == 1 && m.granted(small).size() == 2,
          "past a kind that does not fit, a cheaper kind's jobs still do");
}

void testCoarserTieBreak() {
    WorldService s;
    WorldServiceConfig c;
    c.floorMs = 0.125f;
    s.configure(c);
    const int a = s.addKind({"a", WorldSchedule::Raster, 0.0625f});
    const int b = s.addKind({"b", WorldSchedule::Raster, 0.0625f});
    s.beginFrame();
    // Equal errors: the larger tile first, whichever producer or order.
    s.offer({1, 0, a, 4.0f, 2.0, false});
    s.offer({2, 0, b, 4.0f, 8.0, false});
    s.offer({3, 0, a, 4.0f, 32.0, false});
    s.offer({4, 0, b, 4.0f, 0.5, false});
    s.schedule();
    check(s.granted(a).size() == 1 && s.granted(a)[0].key == 3, "the largest of equal errors goes first");
    check(s.granted(b).size() == 1 && s.granted(b)[0].key == 2, "then the next largest");
    check(s.stats().queued == 2, "the finer two wait");
    // The order never depends on the order of the offers.
    WorldService r;
    r.configure(c);
    r.addKind({"a", WorldSchedule::Raster, 0.0625f});
    r.addKind({"b", WorldSchedule::Raster, 0.0625f});
    r.beginFrame();
    r.offer({4, 0, b, 4.0f, 0.5, false});
    r.offer({3, 0, a, 4.0f, 32.0, false});
    r.offer({2, 0, b, 4.0f, 8.0, false});
    r.offer({1, 0, a, 4.0f, 2.0, false});
    r.schedule();
    check(r.granted(a).size() == 1 && r.granted(a)[0].key == 3 && r.granted(b).size() == 1 &&
              r.granted(b)[0].key == 2,
          "offers in another order are granted alike");
}

void testBudgetFollowsSlack() {
    WorldService s;
    WorldServiceConfig c;
    c.targetMs = 2.78f;
    c.floorMs = 0.1f;
    c.window = 4;
    s.configure(c);
    const int k = s.addKind({"turf", WorldSchedule::Raster, 0.05f});
    // Frames of 1.8 ms, 0.3 of it the service's: the frame needs 1.5.
    for (int i = 0; i < 4; ++i) s.observe(frameOf(1.8, k, 6, 0.3));
    s.beginFrame();
    check(near(s.budgetMs(), 2.78 - 1.5), "the budget is what the frame leaves under the target");
    // The frame grows: the slack follows.
    for (int i = 0; i < 4; ++i) s.observe(frameOf(2.3, k, 2, 0.1));
    s.beginFrame();
    check(near(s.budgetMs(), 2.78 - 2.2), "a heavier frame leaves less");
    // One heavy frame among light ones: the window's heaviest sets it.
    s.observe(frameOf(1.0));
    s.observe(frameOf(1.0));
    s.observe(frameOf(2.0));
    s.observe(frameOf(1.0));
    s.beginFrame();
    check(near(s.budgetMs(), 2.78 - 2.0), "the heaviest of the last frames sets the slack");
    s.observe(frameOf(1.0));
    s.observe(frameOf(1.0));
    s.observe(frameOf(1.0));
    s.beginFrame();
    check(near(s.budgetMs(), 2.78 - 1.0), "past the window it is forgotten");
    // A compute span on its own queue ran beside the frame: it is not taken off.
    for (int i = 0; i < 4; ++i) s.observe(frameOf(1.5, k, 4, 0.4, true));
    s.beginFrame();
    check(near(s.budgetMs(), 2.78 - 1.5), "an async compute span is not the frame's");
    // The target is a setting.
    s.setTarget(16.7f);
    s.beginFrame();
    check(near(s.budgetMs(), 16.7 - 1.5), "a 60 fps target leaves more");
    // A frame with no time read changes nothing.
    s.observe(frameOf(-1.0));
    s.beginFrame();
    check(near(s.budgetMs(), 16.7 - 1.5), "an untimed frame leaves the budget where it was");
    // The view jumped: the old view's slack is forgotten, the floor until a
    // frame of the new one is read, and then that frame's slack.
    s.forgetSlack();
    s.beginFrame();
    check(near(s.budgetMs(), 0.1), "after a cut the budget is the floor");
    s.observe(frameOf(-1.0, k, 4, 0.08));   // (a frame of the old view, read late: its costs only)
    s.beginFrame();
    check(near(s.budgetMs(), 0.1), "a frame from before the cut does not bring the slack back");
    s.observe(frameOf(9.0, k, 4, 0.2));
    s.beginFrame();
    check(near(s.budgetMs(), 16.7 - 8.8), "the new view's first frame read sets it");
}

void testViewCut() {
    using bro::scene::vk::SceneWorldService;
    const bromath::Vec3 eye{0.0f, 2.0f, 0.0f}, fwd{0.0f, 0.0f, -1.0f};
    const float half = 0.5236f;   // a 60 degree view
    check(!SceneWorldService::viewCut({0.05f, 2.0f, -0.1f}, fwd, eye, fwd, half), "a step is not a cut");
    check(!SceneWorldService::viewCut(eye, {0.2f, 0.0f, -1.0f}, eye, fwd, half), "a turn of 11 degrees is not a cut");
    check(SceneWorldService::viewCut(eye, {1.0f, 0.0f, -1.0f}, eye, fwd, half), "a turn of 45 degrees is a cut");
    check(SceneWorldService::viewCut({-300.0f, 420.0f, -300.0f}, fwd, eye, fwd, half), "a jump to the sky is a cut");
}

void testFloorOverTarget() {
    WorldService s;
    WorldServiceConfig c;
    c.targetMs = 2.78f;
    c.floorMs = 0.12f;
    s.configure(c);
    const int k = s.addKind({"turf", WorldSchedule::Raster, 0.05f});
    for (int i = 0; i < 4; ++i) s.observe(frameOf(1.0));
    s.beginFrame();
    check(s.budgetMs() > 1.0f, "a light frame leaves a lot");
    // Over the target, whatever made it so: the floor.
    s.observe(frameOf(3.1, k, 10, 0.5));
    s.beginFrame();
    check(near(s.budgetMs(), 0.12), "a frame over target takes the budget to the floor");
    s.offer({1, 0, k, 3.0f, 1.0, false});
    s.offer({2, 0, k, 2.0f, 1.0, false});
    s.offer({3, 0, k, 1.0f, 1.0, false});
    s.schedule();
    check(s.granted(k).size() == 2, "the floor still grants what fits it (two at 0.05)");
    // Back under it, the slack again (the window's heaviest need: 3.1 - 0.5).
    s.observe(frameOf(1.0));
    s.beginFrame();
    check(near(s.budgetMs(), 2.78 - 2.6), "back under target, the slack returns");
    // Need past the target, though the frame read was under it: the floor.
    WorldService t;
    t.configure(c);
    t.addKind({"turf", WorldSchedule::Raster, 0.05f});
    t.observe(frameOf(2.7));
    t.beginFrame();
    check(near(t.budgetMs(), 0.12), "slack under the floor is the floor");
}

void testCostLearning() {
    WorldService s;
    WorldServiceConfig c;
    c.costRate = 0.25f;
    s.configure(c);
    const int k = s.addKind({"turf", WorldSchedule::Raster, 0.05f});
    const int q = s.addKind({"cards", WorldSchedule::Compute, 0.2f});
    check(near(s.cost(k), 0.05) && !s.measured(k), "a kind's cost is its guess until measured");
    s.observe(frameOf(1.0, k, 10, 0.2));
    check(s.measured(k) && near(s.cost(k), 0.02), "the first measurement replaces the guess (0.2 ms / 10)");
    s.observe(frameOf(1.0, k, 10, 0.6));
    check(near(s.cost(k), 0.02 + 0.25 * (0.06 - 0.02)), "later ones are followed at the rate");
    for (int i = 0; i < 60; ++i) s.observe(frameOf(1.0, k, 4, 0.16));
    check(near(s.cost(k), 0.04, 1e-3), "a steady cost is converged on");
    check(near(s.cost(q), 0.2) && !s.measured(q), "another kind's cost is its own");
    // The budget is spent in the learned costs.
    for (int i = 0; i < 4; ++i) s.observe(frameOf(2.37));   // (0.41 ms of slack)
    s.beginFrame();
    for (uint64_t j = 0; j < 40; ++j) s.offer({j, 0, k, 1.0f + j, 1.0, false});
    s.schedule();
    check(s.granted(k).size() == 10, "0.41 ms of slack at 0.04 a job is ten jobs (got " +
                                         std::to_string(s.granted(k).size()) + ")");
    // A span with no jobs, or no time, teaches nothing.
    const float before = s.cost(k);
    s.observe(frameOf(1.0, k, 0, 0.5));
    s.observe(frameOf(1.0, k, 3, 0.0));
    check(near(s.cost(k), before), "an empty span leaves the cost");
}

}  // namespace

int main() {
    testRankingAcrossProducers();
    testCoarserTieBreak();
    testBudgetFollowsSlack();
    testFloorOverTarget();
    testCostLearning();
    testViewCut();
    std::printf("world service: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
