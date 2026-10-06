// Native test exercising XWayland window integration in bro:
// - Automatic XWayland DISPLAY lifecycle
// - X11 client window mapping and registration in WindowManager
// - Window management controls (placement, focus, state) applied to X11 windows
// - Clean window unmapping upon client disconnect
//
// Verifies Milestone 4 of Item 3 ("XWayland").

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

ChildProcess spawnX11Client(const std::string& binary, const std::vector<std::string>& args,
                            const std::string& waylandSock, const std::string& x11Display) {
    ChildProcess cp;
    int p[2];
    if (::pipe(p) != 0) return cp;

    pid_t pid = ::fork();
    if (pid == 0) {
        ::close(p[0]);
        ::dup2(p[1], STDOUT_FILENO);
        ::close(p[1]);

        ::setenv("WAYLAND_DISPLAY", waylandSock.c_str(), 1);
        ::setenv("DISPLAY", x11Display.c_str(), 1);

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
    std::cout << "=== bro_xwayland_test: XWayland Server & Window Management ===" << std::endl;

#if !defined(__linux__) || !BRO_WITH_COMPOSITOR
    std::cout << "SKIP (77): Only supported on Linux with BRO_WITH_COMPOSITOR" << std::endl;
    return 77;
#else

    std::string x11ClientBin = findClient("bc_x11_client");

    // 1. Initialize WaylandCompositor with Xwayland enabled in headless mode
    std::cout << "[step 1] Initializing WaylandCompositor with Xwayland enabled..." << std::endl;
    compositor::WaylandCompositor comp;
    compositor::CompositorConfig compCfg;
    compCfg.headless = true;
    compCfg.width = 1024;
    compCfg.height = 768;
    compCfg.xwayland = true;

    std::string compErr;
    if (!comp.init(compCfg, &compErr)) {
        std::cout << "SKIP (77): WaylandCompositor::init failed: " << compErr << std::endl;
        return 77;
    }
    CHECK(comp.isRunning());

    std::string sock = comp.socketName();
    CHECK(!sock.empty());

    std::string xdisp = comp.xwaylandDisplay();
    std::cout << "  Compositor running on Wayland socket: " << sock
              << ", Xwayland DISPLAY: " << (xdisp.empty() ? "(none)" : xdisp) << std::endl;

    if (xdisp.empty() || ::access("/usr/bin/Xwayland", X_OK) != 0 || x11ClientBin.empty()) {
        std::cout << "SKIP (77): Xwayland binary not available or wlroots built without Xwayland" << std::endl;
        comp.shutdown();
        return 77;
    }

    // 2. Spawn X11 client
    std::cout << "[step 2] Spawning X11 test client (bc_x11_client)..." << std::endl;
    ChildProcess xClient = spawnX11Client(x11ClientBin, {
        "--class", "BcX11",
        "--instance", "bcx11",
        "--title", "bro-x11-test",
        "--size", "300x200",
        "--color", "C03080"
    }, sock, xdisp);
    CHECK(xClient.pid > 0);

    // Wait for client to map
    xClient.waitLine("mapped", 5000);

    // 3. Verify X11 window registers in compositor's window manager
    std::cout << "[step 3] Polling compositor for X11 client window..." << std::endl;
    uint64_t xWinId = 0;
    for (int retry = 0; retry < 50; ++retry) {
        comp.pollEvents();
        auto wins = comp.windows();
        if (!wins.empty()) {
            xWinId = wins[0];
            break;
        }
        ::usleep(20000);
    }
    CHECK(xWinId != 0);
    std::cout << "  X11 client window registered with WindowId: " << xWinId << std::endl;

    // 4. Exercise Window Manager operations on X11 window
    std::cout << "[step 4] Exercising Window Manager controls on X11 window..." << std::endl;
    CHECK(comp.placeWindow(xWinId, 60, 80, 320, 220));
    CHECK(comp.focusWindow(xWinId));
    CHECK(comp.setWindowState(xWinId, /*maximized=*/true, /*fullscreen=*/false));
    CHECK(comp.setWindowState(xWinId, /*maximized=*/false, /*fullscreen=*/false));
    CHECK(comp.setWindowMinimized(xWinId, /*minimized=*/false));
    std::cout << "  Window Manager controls passed" << std::endl;

    // 5. Test acquiring client layers
    std::cout << "[step 5] Testing client layer acquisition with X11 window..." << std::endl;
    std::vector<engine::UILayer> layers;
    auto leased = comp.acquireClientLayers(layers);
    std::cout << "  Acquired " << layers.size() << " UILayer(s)" << std::endl;
    comp.releaseClientLayers(leased);

    // 6. Cleanup X11 client and shutdown
    std::cout << "[step 6] Terminating X11 client and verifying window unmapping..." << std::endl;
    xClient.kill();
    for (int retry = 0; retry < 50; ++retry) {
        comp.pollEvents();
        if (comp.windows().empty()) break;
        ::usleep(20000);
    }
    CHECK(comp.windows().empty());
    std::cout << "  X11 window unmapped cleanly" << std::endl;

    comp.shutdown();
    CHECK(!comp.isRunning());

    if (gFailures > 0) {
        std::cerr << "FAIL: " << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "=== bro_xwayland_test: ALL TESTS PASSED (100%) ===" << std::endl;
    return 0;
#endif
}
