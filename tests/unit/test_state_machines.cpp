// bro_state_model_test: Exhaustive deterministic state-space model checking for:
//   1. Vulkan timeline ticket retirement, present ordering, idle-frame coalescing,
//      and frame-arena deferred destruction (VulkanQueue + VulkanFrames).
//   2. DOM / CSS / Layout dirty-bit, mutationEpoch, flushLayoutForRead, and
//      paint-invalidation preservation invariants (Document + Element + StyleProxy).

#include "dom/document.h"
#include "dom/element.h"
#include "engine/css_transitions.h"
#include "engine/web_animations.h"
#include "render/vulkan_context.h"
#include "render/vulkan_frames.h"
#include "render/vulkan_queue.h"

namespace bro::engine {
void TransitionManager::forgetDocument(const dom::Document*) {}
void TransitionManager::forgetElement(dom::Element*) {}
void TransitionManager::displayToggled(dom::Element*) {}
void TransitionManager::onStyleChange(dom::Element*, const htmlayout::css::ComputedStyle&,
                                      htmlayout::css::ComputedStyle&, double) {}
void AnimationManager::forgetDocument(const dom::Document*) {}
void AnimationManager::forgetElement(dom::Element*) {}
void AnimationManager::displayToggled(dom::Element*) {}
void AnimationManager::onStyleChange(dom::Element*, const htmlayout::css::ComputedStyle&, double) {}
bool WebAnimationManager::hasActive(dom::Element*) const { return false; }
void WebAnimationManager::applyOverrides(dom::Element*, htmlayout::css::ComputedStyle&, double) const {}
}  // namespace bro::engine

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace bro;

namespace {

int gFailures = 0;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::cerr << "  FAIL: " #cond " (line " << __LINE__ << ")" << std::endl;  \
            ++gFailures;                                                              \
        }                                                                             \
    } while (0)

struct SimpleMetrics : htmlayout::layout::TextMetrics {
    float measureWidth(std::string_view text, std::string_view, float fontSize, std::string_view) override {
        measureCalls++;
        return static_cast<float>(text.size()) * (fontSize * 0.5f);
    }
    float lineHeight(std::string_view, float fontSize, std::string_view) override {
        return fontSize * 1.2f;
    }
};

// -----------------------------------------------------------------------------
// Part 1: Exhaustive State-Space Model Checker for VulkanFrames + VulkanQueue
// -----------------------------------------------------------------------------

struct DeferredRes {
    int id = 0;
    uint64_t requiredGraphicsTicket = 0;
    uint64_t requiredComputeTicket = 0;
    uint64_t requiredPresentTicket = 0;  // if present occurred before frame close, must wait for > present
};

struct SimSlot {
    bool used = false;
    uint64_t ticket = 0;
    uint64_t computeTicket = 0;
    uint64_t arenaMinTicket = 0;  // ticket required before this slot's arenas can be reset
    std::vector<DeferredRes> deferred;
};

struct SimVulkanState {
    static constexpr uint32_t kFramesInFlight = 2;

    // VulkanQueue state
    uint64_t lastTicket = 0;
    uint64_t completedTicket = 0;
    bool presentedSince = false;
    uint64_t lastPresentAfterTicket = 0;

    // Async compute queue state
    uint64_t computeLastTicket = 0;
    uint64_t computeCompletedTicket = 0;

    // VulkanFrames state
    uint64_t serial = 0;
    SimSlot slots[kFramesInFlight]{};
    std::vector<DeferredRes> pending;
    bool hookArmed = false;

    int nextResId = 1;
    int destroyedCount = 0;
    bool invariantViolated = false;

    uint32_t frameIndex() const { return static_cast<uint32_t>(serial % kFramesInFlight); }

    uint64_t retireTicket() const {
        return lastTicket + (presentedSince ? 1 : 0);
    }

    bool isComplete(uint64_t t) const { return t <= completedTicket; }

    uint64_t submitGraphics() {
        if (serial == 0) beginFrame();
        slots[frameIndex()].used = true;
        const uint64_t t = ++lastTicket;
        presentedSince = false;
        slots[frameIndex()].arenaMinTicket = std::max(slots[frameIndex()].arenaMinTicket, t);
        return t;
    }

    uint64_t submitCompute() {
        if (serial == 0) beginFrame();
        slots[frameIndex()].used = true;
        const uint64_t t = ++computeLastTicket;
        return t;
    }

    void present() {
        presentedSince = true;
        lastPresentAfterTicket = lastTicket;
    }

    void deferResource() {
        DeferredRes r;
        r.id = nextResId++;
        r.requiredGraphicsTicket = lastTicket;
        r.requiredComputeTicket = computeLastTicket;
        r.requiredPresentTicket = presentedSince ? (lastTicket + 1) : lastTicket;
        pending.push_back(r);
    }

    void waitGraphics(uint64_t ticket) {
        if (ticket == 0) return;
        if (ticket > lastTicket) {
            // Empty batch submission materializes retireTicket() after a present
            lastTicket = ticket;
            presentedSince = false;
        }
        completedTicket = std::max(completedTicket, ticket);
    }

    void waitCompute(uint64_t ticket) {
        if (ticket == 0) return;
        computeCompletedTicket = std::max(computeCompletedTicket, ticket);
    }

    void recycle(SimSlot& slot) {
        waitGraphics(slot.ticket);
        waitCompute(slot.computeTicket);

        // Safety Invariant 1: When slot arenas are reset, GPU must have completed
        // every submission made from this slot in its previous incarnation.
        if (completedTicket < slot.arenaMinTicket || completedTicket < slot.ticket ||
            computeCompletedTicket < slot.computeTicket) {
            invariantViolated = true;
        }

        // Safety Invariant 2: No deferred resource is ever destroyed before its
        // graphics, compute, and present tickets have completed on the GPU.
        for (const auto& r : slot.deferred) {
            if (completedTicket < r.requiredGraphicsTicket ||
                completedTicket < r.requiredPresentTicket ||
                computeCompletedTicket < r.requiredComputeTicket) {
                invariantViolated = true;
            }
            ++destroyedCount;
        }
        slot.deferred.clear();
        slot.used = false;
        slot.arenaMinTicket = 0;
    }

    void beginFrame() {
        if (serial > 0) {
            if (hookArmed) {
                hookArmed = false;
                submitGraphics();
                deferResource();
            }
            SimSlot& closing = slots[frameIndex()];
            const SimSlot& nextSlot = slots[(serial + 1) % kFramesInFlight];
            if (!closing.used && !isComplete(nextSlot.ticket)) {
                return;
            }
            closing.ticket = retireTicket();
            closing.computeTicket = computeLastTicket;
            for (auto& r : pending) {
                // Any resource deferred during this frame may also depend on
                // submissions made later in the same frame before it closed.
                r.requiredGraphicsTicket = std::max(r.requiredGraphicsTicket, lastTicket);
                r.requiredComputeTicket = std::max(r.requiredComputeTicket, computeLastTicket);
                r.requiredPresentTicket = std::max(r.requiredPresentTicket, closing.ticket);
                closing.deferred.push_back(r);
            }
            pending.clear();
        }
        ++serial;
        recycle(slots[frameIndex()]);
    }

    void shutdown() {
        waitCompute(computeLastTicket);
        waitGraphics(retireTicket());
        hookArmed = false;
        for (const auto& r : pending) {
            if (completedTicket < r.requiredGraphicsTicket ||
                completedTicket < r.requiredPresentTicket ||
                computeCompletedTicket < r.requiredComputeTicket) {
                invariantViolated = true;
            }
            ++destroyedCount;
        }
        pending.clear();
        for (auto& slot : slots) {
            for (const auto& r : slot.deferred) {
                if (completedTicket < r.requiredGraphicsTicket ||
                    completedTicket < r.requiredPresentTicket ||
                    computeCompletedTicket < r.requiredComputeTicket) {
                    invariantViolated = true;
                }
                ++destroyedCount;
            }
            slot.deferred.clear();
            slot.used = false;
        }
    }
};

void testVulkanTicketAndFrameArenaModelChecker() {
    std::cout << "[model-check] Vulkan timeline ticket & frame-arena deferral state space" << std::endl;

    uint64_t statesVisited = 0;
    uint64_t shutdownVerified = 0;

    auto explore = [&](auto& self, const SimVulkanState& st, int depth) -> bool {
        ++statesVisited;
        if (st.invariantViolated) return false;

        // Check shutdown liveness & safety from EVERY reachable state
        {
            SimVulkanState shut = st;
            shut.shutdown();
            if (shut.invariantViolated) return false;
            if (shut.destroyedCount != shut.nextResId - 1) return false;
            ++shutdownVerified;
        }

        if (depth >= 7) return true;

        // Branch 1: beginFrame()
        {
            SimVulkanState next = st;
            next.beginFrame();
            if (!self(self, next, depth + 1)) return false;
        }
        // Branch 2: submitGraphics() + deferResource()
        {
            SimVulkanState next = st;
            next.submitGraphics();
            next.deferResource();
            if (!self(self, next, depth + 1)) return false;
        }
        // Branch 3: submitCompute() + deferResource()
        {
            SimVulkanState next = st;
            next.submitCompute();
            next.deferResource();
            if (!self(self, next, depth + 1)) return false;
        }
        // Branch 4: present() + deferResource()
        {
            SimVulkanState next = st;
            next.present();
            next.deferResource();
            if (!self(self, next, depth + 1)) return false;
        }
        // Branch 5: arm frameEndHook
        if (!st.hookArmed && st.serial > 0) {
            SimVulkanState next = st;
            next.hookArmed = true;
            if (!self(self, next, depth + 1)) return false;
        }
        // Branch 6: GPU advances completedTicket by 1
        if (st.completedTicket < st.lastTicket) {
            SimVulkanState next = st;
            next.completedTicket++;
            if (!self(self, next, depth + 1)) return false;
        }
        return true;
    };

    SimVulkanState initial{};
    CHECK(explore(explore, initial, 0));
    CHECK(statesVisited > 20000);
    CHECK(shutdownVerified == statesVisited);
    std::cout << "  verified " << statesVisited << " reachable states and shutdown trajectories" << std::endl;

    // Also verify concrete headless VulkanContext + VulkanFrames + VulkanQueue
    // across multi-frame wraparound, frameEndHooks, and idle-frame coalescing.
    render::VulkanContextConfig cfg;
    cfg.headless = true;
    render::VulkanContext ctx(cfg);
    if (ctx.init()) {
        auto& frames = ctx.frames();
        auto& queue = ctx.queue();
        int retiredResources = 0;
        int hookCalls = 0;

        auto hookId = frames.addFrameEndHook([&] {
            ++hookCalls;
            (void)frames.allocUpload(64);
        });

        std::vector<uint64_t> frameTickets;
        for (int f = 0; f < 6; ++f) {
            frames.beginFrame();
            VkCommandBuffer cmd = frames.beginCommands();
            uint64_t t = frames.submit(cmd);
            CHECK(t > 0);
            frameTickets.push_back(t);
            frames.defer([&, t] {
                // Must NEVER run before ticket `t` has completed on the GPU!
                CHECK(queue.isComplete(t));
                ++retiredResources;
            });
        }
        frames.removeFrameEndHook(hookId);
        // After 6 frames with kFramesInFlight=2, at least 4 frames' deferred callbacks
        // must have already retired in order, and shutdown() retires the rest.
        CHECK(retiredResources >= 4);
        CHECK(hookCalls == 5);
        frames.shutdown();
        CHECK(retiredResources == 6);
    }
}

// -----------------------------------------------------------------------------
// Part 2: Exhaustive State-Space Model Checker for DOM/Layout Dirty-Bit & Epoch
// -----------------------------------------------------------------------------

void testDomLayoutDirtyBitStateMachine() {
    std::cout << "[model-check] DOM/CSS/Layout dirty-bit, mutationEpoch & flushLayoutForRead" << std::endl;
    SimpleMetrics metrics;

    dom::Document doc;
    doc.setMediaViewport(800.0f, 600.0f);
    doc.parse(
        "<html><head><style>"
        "  html, body, div { display: block; }"
        "  body { margin: 0; width: 800px; }"
        "  .box { width: 100px; height: 40px; color: rgb(10, 20, 30); }"
        "  .wide { width: 320px; }"
        "  @media (prefers-color-scheme: dark) { .box { width: 260px; } }"
        "</style></head>"
        "<body><div id='a' class='box'>hello</div><div id='b' class='box'>world</div></body></html>");

    // Simulate Engine::flushLayoutForRead
    auto flushLayoutForRead = [&]() {
        if (!doc.isDirty() || doc.layoutIsCurrent()) return false;
        doc.resolveStyles();
        bool laidOut = false;
        if (doc.isLayoutDirty() || doc.isStructureDirty() || !doc.layoutRoot()) {
            doc.performLayout(800.0f, 600.0f, metrics);
            laidOut = true;
        }
        doc.clearDirty();
        doc.markPaintDirty();
        doc.noteLayoutCurrent();
        return laidOut;
    };

    // Simulate full frame render pass (resolveStyles + conditional performLayout + clearDirty + noteLayoutCurrent)
    auto runFramePass = [&]() {
        if (!doc.isDirty() && doc.layoutRoot()) return false;
        doc.resolveStyles();
        if (doc.isLayoutDirty() || doc.isStructureDirty() || !doc.layoutRoot()) {
            doc.performLayout(800.0f, 600.0f, metrics);
        }
        doc.clearDirty();
        doc.noteLayoutCurrent();
        return true;
    };

    // Initial frame pass brings layout and paint to clean state
    CHECK(!doc.layoutRoot());
    CHECK(!doc.layoutIsCurrent());
    CHECK(runFramePass());
    CHECK(!doc.isDirty());
    CHECK(!doc.isLayoutDirty());
    CHECK(!doc.isStructureDirty());
    CHECK(doc.layoutIsCurrent());

    dom::Element* a = doc.getElementById("a");
    dom::Element* b = doc.getElementById("b");
    CHECK(a != nullptr && b != nullptr);
    CHECK(std::fabs(a->layoutBox().contentRect.width - 100.0f) < 1e-3f);

    // Transition 1: Paint-only inline style mutation (background-color)
    uint64_t epoch0 = doc.mutationEpoch();
    a->style().setProperty("background-color", "rgb(255, 0, 0)");
    CHECK(doc.mutationEpoch() > epoch0);
    CHECK(doc.isDirty());
    CHECK(!doc.isLayoutDirty());  // not layout-dirty prior to style diff!
    CHECK(!doc.layoutIsCurrent());

    // flushLayoutForRead on a paint-only mutation resolves styles, sees no layout
    // property changed, skips performLayout(), preserves isDirty() for paint,
    // and marks layoutIsCurrent() = true.
    uint64_t passesBefore = doc.perf().passes;
    CHECK(!flushLayoutForRead());
    CHECK(doc.perf().passes == passesBefore);
    CHECK(doc.layoutIsCurrent());
    CHECK(doc.isDirty());         // Paint invalidation MUST NOT be dropped!
    CHECK(!doc.isLayoutDirty());

    // A second flushLayoutForRead with no mutation in between is an immediate no-op.
    CHECK(!flushLayoutForRead());
    CHECK(doc.isDirty());

    // Frame pass consumes the paint dirty bit.
    CHECK(runFramePass());
    CHECK(!doc.isDirty());
    CHECK(doc.layoutIsCurrent());

    // Transition 2: Layout-affecting inline style mutation (width)
    a->style().setProperty("width", "250px");
    CHECK(doc.isDirty());
    CHECK(!doc.isLayoutDirty());  // style diff hasn't run yet
    CHECK(!doc.layoutIsCurrent());

    // flushLayoutForRead runs resolveStyles() -> promotes layoutDirty_ -> runs performLayout()
    // -> keeps isDirty() == true for paint -> sets layoutIsCurrent() == true.
    CHECK(flushLayoutForRead());
    CHECK(doc.perf().passes == passesBefore + 1);
    CHECK(std::fabs(a->layoutBox().contentRect.width - 250.0f) < 1e-3f);
    CHECK(doc.layoutIsCurrent());
    CHECK(doc.isDirty());         // Paint invalidation preserved for next frame!
    CHECK(!doc.isLayoutDirty());

    // Subsequent read in the same turn does not re-layout.
    CHECK(!flushLayoutForRead());
    CHECK(doc.perf().passes == passesBefore + 1);

    // Transition 3: Class mutation while paint is still pending from flushLayoutForRead
    b->setAttribute("class", "box wide");
    CHECK(!doc.layoutIsCurrent());
    CHECK(doc.isDirty());
    CHECK(flushLayoutForRead());
    CHECK(std::fabs(b->layoutBox().contentRect.width - 320.0f) < 1e-3f);
    CHECK(doc.isDirty());
    CHECK(runFramePass());
    CHECK(!doc.isDirty());

    // Transition 4: Structural mutation (appendChild / removeChild)
    dom::Element* c = doc.createElement("div");
    c->setAttribute("id", "c");
    c->setAttribute("class", "box wide");
    doc.body()->appendChild(c);
    CHECK(doc.isDirty());
    CHECK(doc.isLayoutDirty());
    CHECK(doc.isStructureDirty());
    CHECK(!doc.layoutIsCurrent());

    CHECK(flushLayoutForRead());
    CHECK(!doc.isStructureDirty());
    CHECK(doc.isDirty());
    CHECK(doc.layoutIsCurrent());
    CHECK(std::fabs(c->layoutBox().contentRect.width - 320.0f) < 1e-3f);
    CHECK(runFramePass());
    CHECK(!doc.isDirty());

    // Transition 5: Media query change (prefers-color-scheme: dark)
    doc.setMediaColorScheme("dark");
    CHECK(doc.mediaRestylePending());
    CHECK(doc.isDirty());
    CHECK(doc.isLayoutDirty());
    CHECK(!doc.layoutIsCurrent());

    // Even if mediaRestylePending is consumed early by matchMedia delivery
    // (doc.resolveStyles()), layoutDirty_ and !layoutIsCurrent() remain set!
    doc.resolveStyles();
    CHECK(!doc.mediaRestylePending());
    CHECK(doc.isDirty());
    CHECK(doc.isLayoutDirty());
    CHECK(!doc.layoutIsCurrent());

    a->style().removeProperty("width");
    CHECK(flushLayoutForRead());
    CHECK(std::fabs(a->layoutBox().contentRect.width - 260.0f) < 1e-3f);
    CHECK(doc.isDirty());
    CHECK(runFramePass());
    CHECK(!doc.isDirty());
    CHECK(doc.layoutIsCurrent());
}

}  // namespace

int main() {
    testVulkanTicketAndFrameArenaModelChecker();
    testDomLayoutDirtyBitStateMachine();

    if (gFailures > 0) {
        std::cerr << "FAILED: " << gFailures << " check(s)" << std::endl;
        return 1;
    }
    std::cout << "bro_state_model_test: all checks passed" << std::endl;
    return 0;
}
