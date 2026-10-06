// TermSession and inline images: what a program sends is decoded by the
// session's host (broimage), an animation runs on the parser thread with no
// output and no main-thread help (new frames are published as its frames
// come due), and the image quota is the session's to set.

#include "check.h"
#include "tests.h"

#include "terminal/term_session.h"

#include <chrono>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <thread>

namespace bro::terminal::test {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

std::string b64(std::string_view in) {
    static const char* tab = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const uint32_t v = uint32_t(uint8_t(in[i])) << 16 | uint32_t(uint8_t(in[i + 1])) << 8 | uint8_t(in[i + 2]);
        for (int k = 3; k >= 0; --k) out.push_back(tab[(v >> (6 * k)) & 63]);
    }
    if (i < in.size()) {
        uint32_t v = uint32_t(uint8_t(in[i])) << 16;
        if (i + 1 < in.size()) v |= uint32_t(uint8_t(in[i + 1])) << 8;
        out.push_back(tab[(v >> 18) & 63]);
        out.push_back(tab[(v >> 12) & 63]);
        out.push_back(i + 1 < in.size() ? tab[(v >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

bool waitFor(const std::function<bool()>& pred, std::chrono::milliseconds limit = 5s) {
    const auto until = Clock::now() + limit;
    while (Clock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

void testAnimation() {
    section("session images: an animated GIF runs on the parser thread");
    const std::string gif = readFile(std::string(BROPTY_TEST_DATA) + "/images/anim.gif");
    CHECK(!gif.empty());
    TermSession s(40, 10);
    s.resize(40, 10, 10, 20);
    // iTerm2 inline image: three solid frames (red, green, blue), 70 ms each.
    s.feed("\x1b]1337;File=inline=1;size=" + std::to_string(gif.size()) + ":" + b64(gif) + "\x07");
    const TermSession::ImageStats st = s.imageStats();
    CHECK_MSG(st.images == 1, "decoded: " + std::to_string(st.images));
    CHECK(st.bytes == 16u * 12 * 4 * 3 && st.limit == 320u << 20);
    // With nothing fed and nothing asked of it, the session keeps
    // publishing frames as the animation's frames come due: the frame image
    // goes through all three pixel buffers.
    std::set<uint64_t> serials;
    std::set<uint32_t> colours;
    const bool all = waitFor([&] {
        auto f = s.acquireFrame();
        if (f)
            for (const bropty::FrameImage& im : f->images) {
                serials.insert(im.pixels->serial);
                const auto& px = im.pixels->rgba;
                if (px.size() >= 4) colours.insert(uint32_t(px[0]) << 16 | uint32_t(px[1]) << 8 | px[2]);
            }
        return colours.size() >= 3;
    }, 3s);
    CHECK_MSG(all, "frames shown: " + std::to_string(colours.size()) + " colours, " + std::to_string(serials.size()) +
                       " buffers");
    CHECK(colours.count(0xFF0000) && colours.count(0x00FF00) && colours.count(0x0000FF));

    // The quota: lowered to nothing, the image goes, and the frame shows it.
    s.setImageMemoryLimit(0);
    CHECK(s.imageStats().bytes == 0 && s.imageStats().images == 0 && s.imageStats().limit == 0);
    CHECK(waitFor([&] {
        auto f = s.acquireFrame();
        return f && f->images.empty();
    }));
    // And a new one is refused while it is zero.
    s.feed("\x1b]1337;File=inline=1;size=" + std::to_string(gif.size()) + ":" + b64(gif) + "\x07");
    CHECK(s.imageStats().images == 0);
}

} // namespace

void run_session_image_tests() { testAnimation(); }

} // namespace bro::terminal::test
