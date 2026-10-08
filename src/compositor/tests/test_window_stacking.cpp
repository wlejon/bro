// Window stacking in the shell host's compositor, with real clients: one
// order (the window manager's) drives the composite, the hit tests and focus.
//   - a new window maps on top and focused
//   - focusing a window raises it (bro.compositor.focusWindow, a click)
//   - the composite draws windows bottom to top in that order, and a
//     recorded run (render::ClientWindowRef) composites exactly its windows
//   - windowAt answers with the topmost window under the point
//   - closing the focused window focuses the next most recently used one
//   - decoration insets reach stack() for server-side-decorated windows only
//
// Run with stdin open (the test clients exit when it closes):
//   sleep 60 | ./bro_window_stacking_test
#include "compositor/wayland_compositor.h"
#include "engine/ui_layer.h"
#include "render/layer_source.h"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <poll.h>
#include <span>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
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

std::string findClient(const char* name) {
    std::vector<std::string> paths = {
        std::string("../brocompositor/build-release/tests/") + name,
        std::string("/home/j/projects/brocompositor/build-release/tests/") + name,
        std::string("/home/j/projects/brocompositor/build/tests/") + name,
    };
    if (const char* dir = std::getenv("BC_TEST_CLIENT_DIR")) paths.insert(paths.begin(), std::string(dir) + "/" + name);
    for (const auto& p : paths)
        if (::access(p.c_str(), X_OK) == 0) return p;
    return "";
}

struct Client {
    pid_t pid = -1;
    ~Client() { kill(); }
    void kill() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, 0);
            pid = -1;
        }
    }
};

std::unique_ptr<Client> spawn(const std::string& bin, const std::vector<std::string>& args, const std::string& sock) {
    auto c = std::make_unique<Client>();
    pid_t pid = ::fork();
    if (pid == 0) {
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) ::dup2(devnull, STDOUT_FILENO);
        ::setenv("WAYLAND_DISPLAY", sock.c_str(), 1);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(bin.c_str(), argv.data());
        std::_Exit(127);
    }
    c->pid = pid;
    return c;
}

// Polls until `pred` holds (or ~3 s pass).
template <typename F>
bool pump(compositor::WaylandCompositor& comp, F pred) {
    for (int i = 0; i < 150; ++i) {
        comp.pollEvents();
        if (pred()) return true;
        ::usleep(20000);
    }
    return false;
}

std::vector<uint64_t> stackIds(const compositor::WaylandCompositor& comp) {
    std::vector<uint64_t> ids;
    for (const auto& w : comp.stack()) ids.push_back(w.id);
    return ids;
}

uint64_t topId(const compositor::WaylandCompositor& comp) {
    auto ids = stackIds(comp);
    return ids.empty() ? 0 : ids.back();
}

// The window whose buffer is the layer at index i of a composite: matched by
// the layer's quad origin against each window's frame.
std::vector<uint64_t> compositeOrder(compositor::WaylandCompositor& comp,
                                     std::span<const render::ClientWindowRef> refs) {
    std::vector<engine::UILayer> layers;
    auto leased = comp.acquireClientLayers(refs, 0, layers);
    std::vector<uint64_t> order;
    for (const auto& l : layers)
        for (const auto& w : comp.stack())
            if (static_cast<int>(l.quad.x) == w.frame.x && static_cast<int>(l.quad.y) == w.frame.y) {
                order.push_back(w.id);
                break;
            }
    comp.releaseClientLayers(leased);
    return order;
}

}  // namespace

int main() {
    std::cout << "=== bro_window_stacking_test ===" << std::endl;
#if !BRO_HAVE_WAYLAND_SERVER
    std::cout << "SKIP (77): needs brocompositor's Wayland server" << std::endl;
    return 77;
#else
    const std::string bin = findClient("bc_wl_client");
    if (bin.empty()) {
        std::cout << "SKIP (77): bc_wl_client not found" << std::endl;
        return 77;
    }
    compositor::WaylandCompositor comp;
    compositor::CompositorConfig cfg;
    cfg.headless = true;
    cfg.xwayland = false;
    cfg.width = 1280;
    cfg.height = 800;
    std::string err;
    if (!comp.init(cfg, &err)) {
        std::cout << "SKIP (77): compositor init failed: " << err << std::endl;
        return 77;
    }
    const std::string sock = comp.socketName();

    // 1. Three windows, each mapping on top and focused.
    std::cout << "[1] new windows map on top, focused" << std::endl;
    std::vector<std::unique_ptr<Client>> clients;
    std::vector<uint64_t> ids;
    const char* colors[] = {"FFFF0000", "FF00FF00", "FF0000FF"};
    for (int i = 0; i < 3; ++i) {
        std::vector<std::string> args = {"--app-id", "stack" + std::to_string(i), "--size", "300x200",
                                         "--color", colors[i], "--dmabuf"};
        if (i == 2) args.push_back("--csd");
        clients.push_back(spawn(bin, args, sock));
        const size_t want = static_cast<size_t>(i) + 1;
        CHECK(pump(comp, [&] { return comp.stack().size() == want; }));
        auto now = stackIds(comp);
        if (now.size() != want) break;
        uint64_t added = 0;
        for (auto id : now)
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) added = id;
        ids.push_back(added);
        CHECK(topId(comp) == added);
        CHECK(pump(comp, [&] { return comp.focusedWindow() == added; }));
    }
    if (ids.size() != 3) {
        std::cerr << "FAIL: windows did not map" << std::endl;
        return 1;
    }
    const uint64_t a = ids[0], b = ids[1], c = ids[2];
    CHECK((stackIds(comp) == std::vector<uint64_t>{a, b, c}));

    // All three over the same spot, so order is visible to hit tests.
    for (size_t i = 0; i < ids.size(); ++i)
        CHECK(comp.placeWindow(ids[i], 100 + static_cast<int>(i) * 40, 100 + static_cast<int>(i) * 30, 300, 200));
    pump(comp, [&] {
        auto s = comp.queryWindow(c);
        return s && s->frame.x == 180;
    });
    CHECK(comp.windowAt(250, 250) == c);

    // 2. Focus raises: the bottom window goes to the top.
    std::cout << "[2] focus raises" << std::endl;
    CHECK(comp.focusWindow(a));
    comp.pollEvents();
    CHECK((stackIds(comp) == std::vector<uint64_t>{b, c, a}));
    CHECK(comp.focusedWindow() == a);
    CHECK(comp.windowAt(250, 250) == a);
    // The composite follows the same order (an unpinned run of the stack).
    {
        std::vector<render::ClientWindowRef> refs;
        for (auto id : stackIds(comp)) refs.push_back(render::ClientWindowRef{id, false, 0, 0});
        bool ok = pump(comp, [&] { return compositeOrder(comp, refs).size() == 3; });
        CHECK(ok);
        CHECK((compositeOrder(comp, refs) == std::vector<uint64_t>{b, c, a}));
        // A run of one window composites that window only, at a pinned origin.
        std::vector<render::ClientWindowRef> one = {render::ClientWindowRef{c, true, 500, 400}};
        std::vector<engine::UILayer> layers;
        auto leased = comp.acquireClientLayers(one, 0, layers);
        CHECK(layers.size() == 1);
        if (!layers.empty()) CHECK(layers[0].quad.x == 500 && layers[0].quad.y == 400);
        comp.releaseClientLayers(leased);
    }
    // The window manager agrees.
    if (auto* wm = comp.windowManager()) {
        auto st = wm->stacking();
        CHECK((std::vector<uint64_t>(st.begin(), st.end()) == std::vector<uint64_t>{b, c, a}));
    }

    // 3. Focus b, then close a (not focused): b stays focused and on top.
    std::cout << "[3] close restores focus to the most recently used" << std::endl;
    CHECK(comp.focusWindow(b));
    comp.pollEvents();
    CHECK(topId(comp) == b);
    CHECK(comp.focusWindow(c));
    comp.pollEvents();
    CHECK((stackIds(comp) == std::vector<uint64_t>{a, b, c}));
    // Closing c (focused): focus goes back to b, the one used before it.
    clients[2]->kill();
    CHECK(pump(comp, [&] { return comp.stack().size() == 2; }));
    CHECK(pump(comp, [&] { return comp.focusedWindow() == b; }));
    CHECK(topId(comp) == b);

    // 4. Decorations: insets for the server-side-decorated windows only.
    std::cout << "[4] decoration insets" << std::endl;
    if (auto* wm = comp.windowManager()) {
        brocompositor::DecorationConfig deco;
        deco.insets = brocompositor::Margins{6, 36, 6, 6};
        deco.maximized_insets = brocompositor::Margins{0, 36, 0, 0};
        if (auto* be = comp.backend()) be->execute(wm->set_decoration(deco));
        comp.pollEvents();
        for (const auto& w : comp.stack()) {
            CHECK(w.insets.top == 36);
            CHECK(w.insets.left == 6);
        }
        CHECK(comp.setWindowState(a, true, false));
        pump(comp, [&] {
            auto s = comp.queryWindow(a);
            return s && s->maximized && s->frame.y == 36;
        });
        for (const auto& w : comp.stack())
            if (w.id == a) {
                CHECK(w.maximized);
                CHECK(w.insets.top == 36);
                CHECK(w.insets.left == 0);
                // The frame (title bar included) fits the output.
                if (w.frame.y != 36) std::cerr << "  frame " << w.frame.x << "," << w.frame.y << " " << w.frame.width << "x" << w.frame.height << std::endl;
                CHECK(w.frame.y == 36);
                CHECK(w.frame.y + w.frame.height <= 800);
            }
    }

    clients.clear();
    pump(comp, [&] { return comp.stack().empty(); });
    CHECK(comp.stack().empty());
    comp.shutdown();

    if (gFailures > 0) {
        std::cerr << "FAIL: " << gFailures << " check(s) failed" << std::endl;
        return 1;
    }
    std::cout << "=== bro_window_stacking_test: ALL TESTS PASSED ===" << std::endl;
    return 0;
#endif
}
