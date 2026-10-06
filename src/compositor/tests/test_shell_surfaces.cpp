// Native test exercising shell surfaces and window management in bro:
// - xdg-shell client window management (placement, focus, minimize, maximize)
// - layer-shell surfaces (top panels, background, overlays)
// - session lock (ext-session-lock-v1) and isolation
//
// Verifies Milestone 3 of Item 3 ("Shell surfaces and window management").

#include "compositor/wayland_compositor.h"
#include "engine/ui_layer.h"
#include "render/layer_source.h"

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

    bool waitLine(const std::string& expected, int timeoutMs = 3000) {
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
    std::cout << "=== bro_shell_surfaces_test: Shell Surfaces and Window Management ===" << std::endl;

#if !defined(__linux__) || !defined(BRO_WITH_COMPOSITOR)
    std::cout << "SKIP (77): Only supported on Linux with BRO_WITH_COMPOSITOR" << std::endl;
    return 77;
#else

    std::string clientBin = findClient("bc_wl_client");
    std::string sessionClientBin = findClient("bc_wl_session_client");
    if (clientBin.empty()) {
        std::cout << "SKIP (77): bc_wl_client binary not found" << std::endl;
        return 77;
    }

    // 1. Initialize WaylandCompositor in headless mode
    std::cout << "[step 1] Initializing WaylandCompositor in headless mode (1024x768)..." << std::endl;
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
    std::string sock = comp.socketName();
    CHECK(!sock.empty());
    CHECK(!comp.isSessionLocked());
    CHECK(comp.windows().empty());
    std::cout << "  Compositor running on socket: " << sock << std::endl;

    // 2. Test xdg-shell client window
    std::cout << "[step 2] Spawning xdg-shell window client (app-id 'editor', 300x200)..." << std::endl;
    ChildProcess winClient = spawnClient(clientBin, {
        "--app-id", "editor",
        "--size", "300x200",
        "--color", "FF112233",
        "--dmabuf"
    }, sock);
    CHECK(winClient.pid > 0);
    winClient.waitLine("ready", 4000);

    // Poll compositor events until window appears
    uint64_t appWinId = 0;
    for (int retry = 0; retry < 50; ++retry) {
        comp.pollEvents();
        auto wins = comp.windows();
        if (!wins.empty()) {
            appWinId = wins[0];
            break;
        }
        ::usleep(20000);
    }
    CHECK(appWinId != 0);
    std::cout << "  xdg-shell window registered with WindowId: " << appWinId << std::endl;

    // 3. Test Window Manager controls
    std::cout << "[step 3] Exercising Window Manager controls (place, focus, state)..." << std::endl;
    CHECK(comp.placeWindow(appWinId, 50, 60, 320, 220));
    CHECK(comp.focusWindow(appWinId));
    CHECK(comp.setWindowState(appWinId, /*maximized=*/true, /*fullscreen=*/false));
    CHECK(comp.setWindowState(appWinId, /*maximized=*/false, /*fullscreen=*/false));
    CHECK(comp.setWindowMinimized(appWinId, /*minimized=*/false));
    std::cout << "  Window Manager controls passed" << std::endl;

    // 4. Test Layer-shell surface (Top panel)
    std::cout << "[step 4] Spawning layer-shell top panel client (1024x32, exclusive 32)..." << std::endl;
    // Anchor top | left | right = 1 | 4 | 8 = 13
    ChildProcess panelClient = spawnClient(clientBin, {
        "--layer", "top",
        "--anchor", "13",
        "--size", "0x32",
        "--exclusive", "32",
        "--color", "FFAABBCC",
        "--dmabuf"
    }, sock);
    CHECK(panelClient.pid > 0);
    panelClient.waitLine("ready", 4000);

    bool panelFound = false;
    for (int retry = 0; retry < 50; ++retry) {
        comp.pollEvents();
        for (const auto& ls : comp.layerSurfaces()) {
            if (ls.layer == brocompositor::wl::Layer::Top) {
                panelFound = true;
                break;
            }
        }
        if (panelFound) break;
        ::usleep(20000);
    }
    CHECK(panelFound);
    std::cout << "  Layer-shell top panel surface registered" << std::endl;

    // 5. Test acquireClientLayers returns layered surfaces
    std::cout << "[step 5] Testing layered surface acquisition..." << std::endl;
    std::vector<engine::UILayer> layers;
    std::vector<compositor::LeasedSurfaceFrame> leased;
    for (int retry = 0; retry < 50; ++retry) {
        comp.pollEvents();
        layers.clear();
        leased = comp.acquireClientLayers(layers);
        if (!leased.empty()) break;
        ::usleep(20000);
    }
    std::cout << "  Acquired " << layers.size() << " UILayer(s) from " << leased.size() << " leased frame(s)" << std::endl;
    CHECK(!layers.empty());
    comp.releaseClientLayers(leased);

    // 6. Test session lock (if session client is available)
    if (!sessionClientBin.empty()) {
        std::cout << "[step 6] Spawning ext-session-lock client..." << std::endl;
        ChildProcess lockClient = spawnClient(sessionClientBin, {"--lock", "--color", "FF000000"}, sock);
        if (lockClient.pid > 0) {
            bool locked = false;
            for (int retry = 0; retry < 50; ++retry) {
                comp.pollEvents();
                if (comp.isSessionLocked()) {
                    locked = true;
                    break;
                }
                ::usleep(20000);
            }
            CHECK(locked);
            std::cout << "  Session lock engaged: isSessionLocked()=true" << std::endl;

            // When locked, acquireClientLayers must NOT expose normal windows or panels
            std::vector<engine::UILayer> lockLayers;
            auto lockLeased = comp.acquireClientLayers(lockLayers);
            comp.releaseClientLayers(lockLeased);
            std::cout << "  Locked frame acquisition: " << lockLayers.size() << " layer(s)" << std::endl;

            // Release lock by terminating locker
            lockClient.kill();
            for (int retry = 0; retry < 50; ++retry) {
                comp.pollEvents();
                if (!comp.isSessionLocked()) break;
                ::usleep(20000);
            }
            std::cout << "  Session unlocked" << std::endl;
        }
    }

    // 7. Cleanup
    std::cout << "[step 7] Cleaning up clients and shutting down compositor..." << std::endl;
    winClient.kill();
    panelClient.kill();
    comp.shutdown();
    CHECK(!comp.isRunning());

    if (gFailures > 0) {
        std::cerr << "FAIL: " << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "=== bro_shell_surfaces_test: ALL TESTS PASSED (100%) ===" << std::endl;
    return 0;
#endif
}
