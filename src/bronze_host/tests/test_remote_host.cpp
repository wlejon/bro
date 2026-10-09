// bro.remote end to end: bro hosts tests/remote/app through bro.remote.host()
// and this test is the viewer, a broremote Client on the same socket. The
// frames it receives must be the page's pixels (Raw, so exactly); a key and a
// click it sends must arrive as DOM events, which the page shows by changing
// colour (seen here in the stream). Two sessions:
//   headless  bro-headless runs tests/remote/host_remote.js, which hosts and
//             checks the details (the key's name, the click's position, the
//             events and status bro.remote reported); its exit status is the
//             rest of the verdict. Frames are composited on demand.
//   windowed  bro itself (SDL's offscreen video driver, as tests/windowed
//             runs it; a real window on Windows), the page hosting itself;
//             frames are the swapchain presents read back. Skipped when no
//             `bro` is beside the test.
// Exit 0 pass, 1 fail, 77 skip (no bro-headless).
#include <broremote/client.h>
#include <broremote/codec.h>
#include <broremote/stream.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace broremote;
using namespace std::chrono_literals;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
        std::fflush(stdout);
    }
}

constexpr uint32_t kKeyA = 30;        // evdev KEY_A
constexpr uint32_t kBtnLeft = 0x110;  // evdev BTN_LEFT

struct Rgb {
    int r, g, b;
};
constexpr Rgb kIdle{32, 80, 160};  // the page's colours (tests/remote/app)
constexpr Rgb kKeyed{160, 32, 80};
constexpr Rgb kClicked{32, 160, 80};

std::string rgbText(const Rgb& c) {
    return "(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + ")";
}

// What the viewer has seen, filled on the client's reader thread.
struct Viewer {
    std::mutex m;
    std::condition_variable cv;
    std::unique_ptr<Decoder> decoder;
    DecodedFrame picture;
    uint64_t pictures = 0;
    std::string decodeError;
    CursorState cursor;
    uint64_t lastFrameId = 0;
    Client* client = nullptr;  // set once connect() returns

    // Waits up to `timeout` for `pred` (called with the lock held).
    bool waitFor(std::chrono::milliseconds timeout, const std::function<bool()>& pred) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, timeout, pred);
    }
};

// Whether every sampled pixel of the picture is `c` (Raw is exact; the slack
// is for nothing but rounding in the page's own compositing).
bool uniform(const DecodedFrame& f, const Rgb& c, std::string* why) {
    if (!f.ready || f.format != PixelFormat::RGBA8 || f.width == 0 || f.height == 0) {
        if (why) *why = "no RGBA picture";
        return false;
    }
    for (uint32_t y = 0; y < f.height; y += 7) {
        for (uint32_t x = 0; x < f.width; x += 7) {
            const uint8_t* p = f.data.data() + static_cast<size_t>(y) * f.stride + static_cast<size_t>(x) * 4;
            if (std::abs(p[0] - c.r) > 2 || std::abs(p[1] - c.g) > 2 || std::abs(p[2] - c.b) > 2) {
                if (why) {
                    *why = "pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " +
                           rgbText({p[0], p[1], p[2]}) + ", want " + rgbText(c);
                }
                return false;
            }
        }
    }
    return true;
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::unique_ptr<Client> connectViewer(Viewer& v, const std::string& socket, std::string* err) {
    auto stream = connect_local(socket, err);
    if (!stream) return nullptr;
    ClientHandlers h;
    h.on_config = [&v](const StreamConfig& cfg) {
        std::lock_guard<std::mutex> lk(v.m);
        std::string e;
        DecoderConfig dc;
        dc.codec = cfg.codec;
        v.decoder = brovideo::create_decoder(dc, &e);
        if (!v.decoder) v.decodeError = "no decoder: " + e;
    };
    h.on_video = [&v](const VideoPacket& pkt) {
        {
            std::lock_guard<std::mutex> lk(v.m);
            if (v.decoder) {
                DecodedFrame out;
                std::string e;
                if (!v.decoder->decode(pkt.data, out, &e)) {
                    v.decodeError = e;
                } else if (out.ready) {
                    v.picture = std::move(out);
                    ++v.pictures;
                }
            }
            v.lastFrameId = pkt.frame_id;
            if (v.client) v.client->ack(pkt.frame_id);
        }
        v.cv.notify_all();
    };
    h.on_cursor = [&v](const CursorState& c) {
        {
            std::lock_guard<std::mutex> lk(v.m);
            v.cursor = c;
        }
        v.cv.notify_all();
    };
    ClientOptions o;
    o.name = "bro_remote_host_test";
    o.codecs = {Codec::Raw};
    return Client::connect(std::move(stream), std::move(h), o, err);
}

// One session against a bro started with `argv`. `scripted`: the child exits
// by itself once the viewer detaches, and its exit status counts; otherwise
// it is killed.
void runSession(const std::string& label, const std::vector<std::string>& argv, const std::string& socket,
                bool scripted) {
    auto fail = [&](const std::string& what) { check(false, label + ": " + what); };
    std::string err;
    auto child = Process::spawn(argv, &err);
    if (!child) return fail("spawning " + argv[0] + ": " + err);

    // Connect once the page is up and hosting (it compiles first).
    Viewer v;
    std::unique_ptr<Client> client;
    const auto deadline = std::chrono::steady_clock::now() + 90s;
    while (!client && std::chrono::steady_clock::now() < deadline) {
        int code = 0;
        if (child->wait_for(0ms, &code)) return fail("bro exited (" + std::to_string(code) + ") before hosting");
        client = connectViewer(v, socket, &err);
        if (!client) std::this_thread::sleep_for(100ms);
    }
    if (!client) {
        child->kill();
        return fail("could not connect to '" + socket + "': " + err);
    }
    {
        // Frames that came before connect() returned are acked now: unacked,
        // they would hold the server's flow-control window shut.
        std::lock_guard<std::mutex> lk(v.m);
        v.client = client.get();
        if (v.lastFrameId) client->ack(v.lastFrameId);
    }

    auto expectColour = [&](const Rgb& want, const char* when) {
        std::string why;
        const bool ok = v.waitFor(20s, [&] { return v.pictures > 0 && uniform(v.picture, want, &why); });
        if (!ok) fail(std::string(when) + ": no frame of " + rgbText(want) + " within 20 s (" + why + ")");
        return ok;
    };

    bool ok = expectColour(kIdle, "the page as first hosted");
    {
        std::lock_guard<std::mutex> lk(v.m);
        if (v.picture.width != 320 || v.picture.height != 240) {
            fail("frame size " + std::to_string(v.picture.width) + "x" + std::to_string(v.picture.height) +
                 ", want 320x240");
        }
        if (!v.decodeError.empty()) fail("decode error: " + v.decodeError);
    }
    if (ok) {
        client->send_input(InputEvent::key(kKeyA, true));
        client->send_input(InputEvent::key(kKeyA, false));
        ok = expectColour(kKeyed, "after the key");
    }
    if (ok) {
        client->send_input(InputEvent::motion(100.0f, 60.0f));
        client->send_input(InputEvent::button(kBtnLeft, true));
        client->send_input(InputEvent::button(kBtnLeft, false));
        ok = expectColour(kClicked, "after the click");
        // The pointer the server reports follows the injected motion.
        if (!v.waitFor(5s, [&] { return v.cursor.x == 100 && v.cursor.y == 60; })) {
            fail("cursor at (" + std::to_string(v.cursor.x) + ", " + std::to_string(v.cursor.y) +
                 "), want (100, 60)");
        }
    }

    // Detaching ends the script's wait; its asserts are the other half.
    client.reset();
    if (!scripted) {
        child->kill();
        child->wait_for(10s);
        return;
    }
    int code = -1;
    if (!child->wait_for(60s, &code)) {
        child->kill();
        return fail("bro-headless did not exit within 60 s of the viewer detaching");
    }
    if (code != 0) fail("bro-headless exited " + std::to_string(code) + " (its asserts above)");
}

}  // namespace

int main(int argc, char** argv) {
    // bro-headless (and bro) sit beside this test (run_tests.sh's $BRO_DIR).
    const fs::path self = fs::absolute(fs::path(argc > 0 ? argv[0] : "bro_remote_host_test"));
    const std::string exe = self.extension().string();  // ".exe" on Windows
    const fs::path headless = self.parent_path() / ("bro-headless" + exe);
    const fs::path windowed = self.parent_path() / ("bro" + exe);
    if (!fs::exists(headless)) {
        std::printf("SKIP: no bro-headless beside the test (%s)\n", headless.string().c_str());
        return 77;
    }
    const fs::path dir = BRO_REMOTE_TEST_DIR;
    // bro.remote is privileged: the app asks for it (bro.json "permissions")
    // and the test names its folder trusted (docs/desktop-trust.md).
    setEnv("BRO_TRUSTED_APP_DIR", (dir / "app").string());
    std::random_device rd;
    const std::string id = std::to_string(rd() % 1000000);

    std::string socket = "bro-remote-test-" + id;
    setEnv("BRO_REMOTE_TEST_SOCKET", socket);
    runSession("headless",
               {headless.string(), "--width", "320", "--height", "240", (dir / "app").string(),
                (dir / "host_remote.js").string()},
               socket, true);

    // is_regular_file: in an embedding build (helm) "bro" beside the test is
    // bro's binary directory, not the executable.
    if (fs::is_regular_file(windowed)) {
        socket = "bro-remote-test-w" + id;
        setEnv("BRO_REMOTE_TEST_SOCKET", socket);
        setEnv("BRO_REMOTE_TEST_WINDOWED", "1");
        setEnv("SDL_AUDIODRIVER", "dummy");
#if !defined(_WIN32)
        // Windows drivers have no VK_EXT_headless_surface for SDL's offscreen
        // driver, so there the window is a real (small, brief) one.
        setEnv("SDL_VIDEODRIVER", "offscreen");
#endif
        runSession("windowed", {windowed.string(), "--no-splash", (dir / "app").string()}, socket, false);
    } else {
        std::printf("note: no bro beside the test; the windowed session is skipped\n");
    }

    if (g_failures == 0) std::printf("PASS: bro.remote hosted from bro-headless and bro\n");
    return g_failures == 0 ? 0 : 1;
}
