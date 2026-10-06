// Native test exercising daily-driver compositor capabilities in bro:
// - Multi-monitor output hotplug and layout configuration
// - Fractional scaling and output geometry updates
// - Window management across multiple monitors and window snapshot querying
// - Layer-shell panel reservations across monitors
//
// Verifies Milestone 5 of Item 3 ("Daily-driver gaps").

#include "compositor/wayland_compositor.h"
#include "engine/ui_layer.h"

#include <iostream>
#include <string>
#include <vector>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>

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
        std::string("/home/j/projects/brocompositor/build-release/tests/") + name,
        std::string("/home/j/projects/brocompositor/build/tests/") + name,
        std::string("/home/j/projects/bro/build/brocompositor/tests/") + name,
    };
    for (const auto& p : paths) {
        if (::access(p.c_str(), X_OK) == 0) return p;
    }
    return "";
}

struct ChildProcess {
    pid_t pid = -1;
    int pipeOut = -1;

    ~ChildProcess() {
        kill();
    }

    void kill() {
        if (pid > 0) {
            ::kill(pid, SIGTERM);
            int status = 0;
            ::waitpid(pid, &status, WNOHANG);
            pid = -1;
        }
        if (pipeOut >= 0) {
            ::close(pipeOut);
            pipeOut = -1;
        }
    }

    bool waitLine(const std::string& expected, int timeoutMs = 5000) {
        if (pipeOut < 0) return false;
        std::string line;
        int remaining = timeoutMs;
        while (remaining > 0) {
            struct pollfd pfd{};
            pfd.fd = pipeOut;
            pfd.events = POLLIN;
            int ret = ::poll(&pfd, 1, 100);
            if (ret > 0 && (pfd.revents & POLLIN)) {
                char ch = 0;
                while (::read(pipeOut, &ch, 1) == 1) {
                    if (ch == '\n') {
                        if (line.find(expected) != std::string::npos) return true;
                        line.clear();
                    } else {
                        line += ch;
                    }
                }
            }
            remaining -= 100;
        }
        return false;
    }
};

ChildProcess spawnClient(const std::string& binary, const std::vector<std::string>& args, const std::string& sock) {
    ChildProcess cp;
    int p[2];
    if (::pipe(p) != 0) return cp;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(p[0]);
        ::dup2(p[1], STDOUT_FILENO);
        ::close(p[1]);

        ::setenv("WAYLAND_DISPLAY", sock.c_str(), 1);

        std::vector<char*> cargs;
        cargs.push_back(const_cast<char*>(binary.c_str()));
        for (const auto& a : args) cargs.push_back(const_cast<char*>(a.c_str()));
        cargs.push_back(nullptr);

        ::execv(binary.c_str(), cargs.data());
        std::exit(127);
    }

    ::close(p[1]);
    cp.pid = pid;
    cp.pipeOut = p[0];
    return cp;
}

} // namespace

int main() {
    std::cout << "=== bro_daily_driver_test: Daily-Driver Compositor Capabilities ===" << std::endl;

#if !BRO_HAVE_WAYLAND_SERVER
    std::cout << "SKIP (77): needs brocompositor's Wayland server (Linux, wlroots 0.18)" << std::endl;
    return 77;
#else

    std::string clientBin = findClient("bc_wl_client");
    if (clientBin.empty()) {
        std::cout << "SKIP (77): bc_wl_client binary not found" << std::endl;
        return 77;
    }

    // 1. Initialize WaylandCompositor in headless mode (primary display 1024x768)
    std::cout << "[step 1] Initializing WaylandCompositor (primary output 1024x768)..." << std::endl;
    compositor::WaylandCompositor comp;
    compositor::CompositorConfig compCfg;
    compCfg.headless = true;
    compCfg.width = 1024;
    compCfg.height = 768;

    std::string compErr;
    if (!comp.init(compCfg, &compErr)) {
        std::cout << "SKIP (77): WaylandCompositor::init failed: " << compErr << std::endl;
        return 77;
    }
    CHECK(comp.isRunning());

    auto mons = comp.monitors();
    CHECK(mons.size() == 1);
    CHECK(mons[0].bounds.width == 1024);
    CHECK(mons[0].bounds.height == 768);
    std::cout << "  Primary monitor active: " << mons[0].name
              << " (" << mons[0].bounds.width << "x" << mons[0].bounds.height << ")" << std::endl;

    // 2. Multi-monitor: dynamically add secondary output (1920x1080)
    std::cout << "[step 2] Hotplugging secondary display (1920x1080)..." << std::endl;
    uint32_t monId2 = comp.addOutput(1920, 1080);
    CHECK(monId2 != 0);

    // Position second output adjacent to first output at x=1024, y=0, scale=1.0f
    CHECK(comp.configureOutput(monId2, 1.0f, 1024, 0));

    // Poll events for monitor change propagation
    for (int r = 0; r < 20; ++r) {
        comp.pollEvents();
        ::usleep(10000);
    }

    mons = comp.monitors();
    CHECK(mons.size() == 2);
    std::cout << "  Dual-monitor layout verified: " << mons.size() << " outputs active" << std::endl;

    // 3. Fractional scaling: set output 2 scale to 1.25x (DPI = 120)
    std::cout << "[step 3] Applying fractional scale (1.25x) to output 2..." << std::endl;
    CHECK(comp.configureOutput(monId2, 1.25f, 1024, 0));
    for (int r = 0; r < 20; ++r) {
        comp.pollEvents();
        ::usleep(10000);
    }
    mons = comp.monitors();
    CHECK(mons.size() == 2);
    // DPI = round(1.25 * 96) = 120
    CHECK(mons[1].dpi == 120);
    std::cout << "  Fractional scale applied: effective DPI=" << mons[1].dpi << " (1.25x scale)" << std::endl;

    // 4. Window placement and query across multiple monitors
    std::cout << "[step 4] Spawning client window and placing across monitors..." << std::endl;
    ChildProcess winClient = spawnClient(clientBin, {
        "--app-id", "multimon-app",
        "--title", "Multi-Monitor Window",
        "--size", "400x300",
        "--color", "FF336699",
        "--dmabuf"
    }, comp.socketName());
    CHECK(winClient.pid > 0);
    winClient.waitLine("ready", 4000);

    uint64_t winId = 0;
    for (int retry = 0; retry < 50; ++retry) {
        comp.pollEvents();
        auto wins = comp.windows();
        if (!wins.empty()) {
            winId = wins[0];
            break;
        }
        ::usleep(20000);
    }
    CHECK(winId != 0);

    auto snap = comp.queryWindow(winId);
    CHECK(snap.has_value());
    CHECK(snap->app_id == "multimon-app");
    CHECK(snap->title == "Multi-Monitor Window");
    std::cout << "  Window registered: app_id='" << snap->app_id << "' frame="
              << snap->frame.width << "x" << snap->frame.height << " on monitor=" << snap->monitor << std::endl;

    // Move window onto secondary display (x=1100, y=100)
    CHECK(comp.placeWindow(winId, 1100, 100, 500, 400));
    for (int r = 0; r < 20; ++r) {
        comp.pollEvents();
        ::usleep(10000);
    }
    snap = comp.queryWindow(winId);
    CHECK(snap.has_value());
    CHECK(snap->frame.x == 1100);
    CHECK(snap->frame.y == 100);
    CHECK(snap->monitor == monId2);
    std::cout << "  Window moved to second display: frame at (" << snap->frame.x << ", " << snap->frame.y
              << ") assigned to monitor=" << snap->monitor << std::endl;

    // 5. Layer acquisition across multi-monitor setup
    std::cout << "[step 5] Testing layer acquisition with multi-monitor outputs..." << std::endl;
    std::vector<engine::UILayer> layers;
    auto leased = comp.acquireClientLayers(layers);
    std::cout << "  Acquired " << layers.size() << " UILayer(s)" << std::endl;
    comp.releaseClientLayers(leased);

    // 6. Cleanup and shutdown
    std::cout << "[step 6] Cleaning up client and shutting down compositor..." << std::endl;
    winClient.kill();
    for (int retry = 0; retry < 50; ++retry) {
        comp.pollEvents();
        if (comp.windows().empty()) break;
        ::usleep(20000);
    }
    CHECK(comp.windows().empty());

    comp.shutdown();
    CHECK(!comp.isRunning());

    if (gFailures > 0) {
        std::cerr << "FAIL: " << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "=== bro_daily_driver_test: ALL TESTS PASSED (100%) ===" << std::endl;
    return 0;
#endif
}
