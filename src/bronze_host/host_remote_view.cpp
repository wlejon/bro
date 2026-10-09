// <remoteview>: a bro.remote.connect() session's screen (host_remote_view.h).
//
// Pictures. broremote's ViewerSession decodes on its own thread. On Windows,
// where the D3D11 bridge works (render/remote_picture.h), sessions ask for
// pictures left on the GPU and the bridge converts each one, on the decode
// thread, into a shared texture Vulkan has imported: the picture the
// session publishes carries the converted texture, and the frame pump only
// swaps which one the view shows (the old one is let go of once the frames
// that sampled it are done). No byte of a picture is read back to the CPU.
// Elsewhere (and when the decoder's adapter is not bro's) pictures come in
// CPU memory and are uploaded into the view's image.
//
// Input (engine/remote_view_host.h): keys by evdev code, the pointer in
// stream pixels through the letterbox, the wheel in 120ths of a detent; the
// release chord (default Ctrl+Alt+Escape) is kept here and un-captures the
// view. While the remote screen holds its pointer locked, motion goes as
// relative motion.

#include "bronze_host/host_remote_view.h"

#include "bronze_host/host_events.h"
#include "bronze_host/host_node.h"
#include "bronze_host/host_runtime.h"

#include "dom/element.h"
#include "dom/event.h"
#include "dom/event_dispatch.h"
#include "engine/engine.h"
#include "engine/key_mapping.h"
#include "engine/remote_view_host.h"
#include "layout/el_remote_view.h"
#include "platform/evdev_keymap.h"
#include "platform/keys.h"
#include "render/remote_picture.h"
#include "render/vulkan_context.h"
#include "util/log.h"

#include <broremote/api.h>
#include <broremote/latency.h>
#include <broremote/viewer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

namespace bro::bronze_host {

namespace {

using broremote::InputEvent;

constexpr const char* kDefaultReleaseChord = "Ctrl+Alt+Escape";

struct Chord {
    platform::KeyMods mods = 0;  // kmod::Ctrl / Alt / Shift / Gui: each one needed
    std::string code;            // the key's KeyboardEvent.code
};

// "Ctrl+Alt+Escape", "Shift+F12", "Meta+KeyQ": modifiers, then a key by
// its KeyboardEvent.code (a letter or digit may be given bare: "Q", "5").
bool parseChord(const std::string& text, Chord& out) {
    Chord c;
    size_t start = 0;
    while (start <= text.size()) {
        size_t plus = text.find('+', start);
        std::string part = text.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
        start = plus == std::string::npos ? text.size() + 1 : plus + 1;
        if (part.empty()) return false;
        std::string lower = part;
        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (plus != std::string::npos) {
            if (lower == "ctrl" || lower == "control") c.mods |= platform::kmod::Ctrl;
            else if (lower == "alt" || lower == "option") c.mods |= platform::kmod::Alt;
            else if (lower == "shift") c.mods |= platform::kmod::Shift;
            else if (lower == "meta" || lower == "super" || lower == "cmd" || lower == "win") c.mods |= platform::kmod::Gui;
            else return false;
            continue;
        }
        if (part.size() == 1 && std::isalpha(static_cast<unsigned char>(part[0])))
            c.code = std::string("Key") + static_cast<char>(std::toupper(static_cast<unsigned char>(part[0])));
        else if (part.size() == 1 && std::isdigit(static_cast<unsigned char>(part[0])))
            c.code = "Digit" + part;
        else if (lower == "esc")
            c.code = "Escape";
        else
            c.code = part;
    }
    if (c.code.empty()) return false;
    out = c;
    return true;
}

struct Letterbox {
    float x = 0, y = 0, w = 0, h = 0;
};

Letterbox letterbox(float outW, float outH, uint32_t sw, uint32_t sh) {
    Letterbox r;
    if (outW <= 0 || outH <= 0 || sw == 0 || sh == 0) return r;
    const float s = std::min(outW / float(sw), outH / float(sh));
    r.w = float(sw) * s;
    r.h = float(sh) * s;
    r.x = (outW - r.w) / 2;
    r.y = (outH - r.h) / 2;
    return r;
}

struct View {
    uint64_t id = 0;
    broremote::ViewerSession* session = nullptr;
    std::unique_ptr<ev::Persistent> sessionObject;

    // The picture shown: a bridged one (GPU path) or the uploader's image.
    broremote::DecodedFrame frame;  // take_frame's buffer, reused
    broremote::ViewerFrameInfo info;
    std::shared_ptr<const render::BridgedPicture> shown;
    std::unique_ptr<render::RemotePictureUploader> uploader;
    bool showUploaded = false;
    uint64_t serial = 0;
    uint64_t presentPending = 0;  // the frame id of a picture not yet presented (0: none)
    std::string unshownWhy;       // why the last picture could not be shown

    uint64_t cursorChanges = 0;
    broremote::CursorState cursor;

    std::set<uint32_t> keys, buttons;  // evdev codes held down through the view
    float wheelX = 0, wheelY = 0;
    bool havePos = false;
    float lastX = -1, lastY = -1;
    bool wasCaptured = false;

    std::string chordText = kDefaultReleaseChord;
    Chord chord;

    uint64_t pictures = 0, bridged = 0, uploads = 0, presented = 0, unshown = 0;
};

struct State {
    engine::Engine* engine = nullptr;
    std::unordered_map<uint64_t, std::unique_ptr<View>> views;
    std::shared_ptr<render::D3D11PictureBridge> bridge;
    bool bridgeTried = false;
    std::string bridgeError;
};
State g;

// Let go of a bridged picture once the GPU frames that may sample it are done.
void releaseLater(std::shared_ptr<const render::BridgedPicture> pic) {
    if (!pic) return;
    render::VulkanContext* ctx = g.engine ? g.engine->vulkanContext() : nullptr;
    if (ctx && ctx->isValid()) ctx->frames().defer([pic = std::move(pic)]() mutable { pic.reset(); });
}

View& viewFor(uint64_t id) {
    auto& slot = g.views[id];
    if (!slot) {
        slot = std::make_unique<View>();
        slot->id = id;
        parseChord(slot->chordText, slot->chord);
    }
    return *slot;
}

View* findView(uint64_t id) {
    auto it = g.views.find(id);
    return it == g.views.end() ? nullptr : it->second.get();
}

void fireEvent(layout::ElRemoteView& rv, const char* type) {
    dom::Element* el = rv.element();
    if (!el) return;
    dom::Event evt(type, false, false);
    evt.setIsTrusted(true);
    dom::dispatchDomEvent(el, evt);
}

void send(View& v, const InputEvent& e) {
    if (v.session) v.session->send_input(e);
}

void releaseHeld(View& v) {
    for (uint32_t k : v.keys) send(v, InputEvent::key(k, false));
    for (uint32_t b : v.buttons) send(v, InputEvent::button(b, false));
    v.keys.clear();
    v.buttons.clear();
    v.wheelX = v.wheelY = 0;
}

void unbind(View& v) {
    releaseHeld(v);
    v.session = nullptr;
    v.sessionObject.reset();
    releaseLater(std::move(v.shown));
    v.shown.reset();
    v.showUploaded = false;
    v.presentPending = 0;
    v.cursorChanges = 0;
    v.cursor = {};
    v.havePos = false;
    ++v.serial;
    if (auto* rv = layout::ElRemoteView::byId(v.id)) rv->setCursorCss("");
}

void ensureBridge() {
    if (g.bridgeTried) return;
    g.bridgeTried = true;
    render::VulkanContext* ctx = g.engine ? g.engine->vulkanContext() : nullptr;
    if (!ctx || !ctx->isValid()) {
        g.bridgeError = "no Vulkan device";
        return;
    }
    g.bridge = render::D3D11PictureBridge::create(*ctx, &g.bridgeError);
    if (g.bridge) LOG_INFO("remoteview: decoded pictures go to the compositor on the GPU (D3D11 -> Vulkan)");
    else LOG_INFO("remoteview: pictures are uploaded from the CPU (%s)", g.bridgeError.c_str());
}

// ---- the session hooks (broremote_api) -------------------------------------

void sessionStarting(broremote::ViewerSession*, broremote::ViewerOptions& options) {
    ensureBridge();
    if (!g.bridge) return;  // Cpu pictures, the default
    std::weak_ptr<render::D3D11PictureBridge> weak = g.bridge;
    options.output = brovideo::PictureMemory::D3D11;
    options.prepare = [weak, failures = uint64_t(0)](broremote::DecodedFrame& pic) mutable {
        if (pic.memory != brovideo::PictureMemory::D3D11 || !pic.gpu.texture) return;
        auto bridge = weak.lock();
        if (!bridge) return;
        std::string err;
        auto converted = bridge->convert(pic.gpu.device, pic.gpu.texture, pic.gpu.subresource, pic.gpu.x, pic.gpu.y,
                                         pic.width, pic.height, &err);
        if (!converted) {
            if (++failures <= 5) LOG_WARN("remoteview: a picture could not be shown: %s", err.c_str());
            return;
        }
        // The decoder's surface goes back now; the picture is the converted
        // texture (device and texture null say so).
        pic.gpu.device = nullptr;
        pic.gpu.texture = nullptr;
        pic.gpu.keepalive = std::const_pointer_cast<render::BridgedPicture>(converted);
    };
    options.read_marker = [weak](const broremote::DecodedFrame& pic) -> int64_t {
        auto bridge = weak.lock();
        if (!bridge || !pic.gpu.texture) return -1;
        uint32_t xs[broremote::kMarkerBits], ys[broremote::kMarkerBits];
        for (uint32_t i = 0; i < broremote::kMarkerBits; ++i) broremote::marker_point(i, xs[i], ys[i]);
        uint8_t luma[broremote::kMarkerBits];
        if (!bridge->readLuma(pic.gpu.device, pic.gpu.texture, pic.gpu.subresource, pic.gpu.x, pic.gpu.y, xs, ys,
                              broremote::kMarkerBits, luma))
            return -1;
        return broremote::marker_from_luma(luma);
    };
}

void sessionGone(broremote::ViewerSession* session) {
    for (auto& [id, v] : g.views) {
        if (v->session == session) unbind(*v);
    }
}

// ---- the frame pump ----------------------------------------------------------

void takePicture(View& v, layout::ElRemoteView& rv) {
    if (!v.session->take_frame(v.frame, v.info)) return;
    ++v.pictures;
    broremote::DecodedFrame& f = v.frame;
    if (f.memory == brovideo::PictureMemory::D3D11) {
        if (f.gpu.texture || !f.gpu.keepalive) {
            ++v.unshown;  // the bridge could not convert it
            f.gpu = {};
            return;
        }
        auto pic = std::static_pointer_cast<const render::BridgedPicture>(f.gpu.keepalive);
        f.gpu = {};
        releaseLater(std::move(v.shown));
        v.shown = std::move(pic);
        v.showUploaded = false;
        ++v.bridged;
    } else {
        render::VulkanContext* ctx = g.engine->vulkanContext();
        if (!ctx || !ctx->isValid() || f.data.empty()) {
            ++v.unshown;
            return;
        }
        if (!v.uploader) v.uploader = std::make_unique<render::RemotePictureUploader>(*ctx);
        const auto img = v.uploader->upload(f.data.data(), f.stride, f.uv_offset,
                                            static_cast<render::RemotePixels>(f.format), f.width, f.height);
        if (!img) {
            ++v.unshown;
            return;
        }
        releaseLater(std::move(v.shown));
        v.shown.reset();
        v.showUploaded = true;
        ++v.uploads;
    }
    ++v.serial;
    v.presentPending = v.info.frame_id ? v.info.frame_id : 0;
    (void)rv;
}

void pump() {
    if (g.bridge) g.bridge->importPending();
    std::vector<uint64_t> gone;
    std::vector<std::pair<uint64_t, const char*>> events;  // fired after the walk (listeners may bind views)
    for (auto& [id, vp] : g.views) {
        View& v = *vp;
        layout::ElRemoteView* rv = layout::ElRemoteView::byId(id);
        if (!rv) {
            gone.push_back(id);
            continue;
        }
        // Capture changes (a press captured it; blur or the chord released it).
        if (rv->captured() != v.wasCaptured) {
            v.wasCaptured = rv->captured();
            if (!v.wasCaptured) releaseHeld(v);
            events.emplace_back(id, v.wasCaptured ? "capture" : "release");
        }
        if (!v.session) continue;

        const broremote::ViewerStatus st = v.session->status();
        if (st.have_config && rv->setStreamSize(st.config.width, st.config.height)) {
            if (dom::Element* el = rv->element()) el->markDirty();
        }
        takePicture(v, *rv);

        broremote::CursorState c;
        const uint64_t changes = v.session->cursor(c);
        if (changes != v.cursorChanges) {
            v.cursorChanges = changes;
            v.cursor = c;
            rv->setCursorCss(!c.visible || c.locked ? "none" : (c.shape.empty() ? "default" : c.shape));
            g.engine->refreshHoverCursor();
        }
    }
    for (uint64_t id : gone) {
        if (View* v = findView(id)) {
            unbind(*v);
            g.views.erase(id);
        }
    }
    for (const auto& [id, type] : events) {
        if (layout::ElRemoteView* rv = layout::ElRemoteView::byId(id)) fireEvent(*rv, type);
    }
}

// ---- the engine's RemoteViewHost --------------------------------------------

class Host final : public engine::RemoteViewHost {
public:
    render::LayerImage layerImage(uint64_t viewId, uint64_t& serial) override {
        View* v = findView(viewId);
        if (!v) return {};
        serial = v->serial;
        if (v->shown && g.bridge) return g.bridge->image(*v->shown);
        if (v->showUploaded && v->uploader) return v->uploader->image();
        return {};
    }

    void presented(const uint64_t* viewIds, size_t count) override {
        const auto now = broremote::Clock::now();
        for (size_t i = 0; i < count; ++i) {
            View* v = findView(viewIds[i]);
            if (!v || !v->presentPending) continue;
            ++v->presented;
            if (v->session) v->session->note_presented(v->presentPending, now);
            v->presentPending = 0;
        }
    }

    bool key(layout::ElRemoteView& rv, int scancode, int mod, bool pressed, bool repeat) override {
        View& v = viewFor(rv.viewId());
        if (pressed && !repeat && !v.chord.code.empty() && engine::sdlScancodeToWebCode(scancode) == v.chord.code &&
            chordModsHeld(v.chord.mods, mod)) {
            rv.setCaptured(false);  // the pump lets go of what is held and says so
            return true;
        }
        const uint32_t code = platform::scancodeToEvdevKey(scancode);
        if (!code) return true;
        if (pressed) {
            if (repeat) return true;  // the remote screen repeats keys itself
            if (v.keys.insert(code).second) send(v, InputEvent::key(code, true));
        } else if (v.keys.erase(code)) {
            send(v, InputEvent::key(code, false));
        }
        return true;
    }

    void pointer(layout::ElRemoteView& rv, float x, float y, float w, float h, float xrel, float yrel, int button,
                 bool pressed) override {
        View& v = viewFor(rv.viewId());
        if (!v.session) return;
        const uint32_t sw = rv.streamWidth(), sh = rv.streamHeight();
        const Letterbox r = letterbox(w, h, sw, sh);
        if (r.w <= 0 || r.h <= 0) return;
        if (v.cursor.locked && rv.captured()) {
            if (button == 0) {
                if (xrel != 0 || yrel != 0)
                    send(v, InputEvent::relative(xrel * float(sw) / r.w, yrel * float(sh) / r.h));
                return;
            }
        } else {
            const float sx = std::clamp((x - r.x) * float(sw) / r.w, 0.0f, float(sw - 1));
            const float sy = std::clamp((y - r.y) * float(sh) / r.h, 0.0f, float(sh - 1));
            if (!v.havePos || sx != v.lastX || sy != v.lastY) {
                v.havePos = true;
                v.lastX = sx;
                v.lastY = sy;
                send(v, InputEvent::motion(sx, sy));
            }
        }
        if (button == 0) return;
        const uint32_t code = platform::mouseButtonToEvdevButton(button);
        if (!code) return;
        if (pressed ? v.buttons.insert(code).second : v.buttons.erase(code) > 0)
            send(v, InputEvent::button(code, pressed));
    }

    void wheel(layout::ElRemoteView& rv, float dx, float dy) override {
        View& v = viewFor(rv.viewId());
        if (!v.session) return;
        // bro: +y away from the user. The protocol is libinput's: +y down.
        v.wheelX += dx * 120.0f;
        v.wheelY += -dy * 120.0f;
        const float ix = std::trunc(v.wheelX), iy = std::trunc(v.wheelY);
        v.wheelX -= ix;
        v.wheelY -= iy;
        if (ix != 0 || iy != 0) send(v, InputEvent::wheel(int32_t(ix), int32_t(iy)));
    }

    void releaseAll(layout::ElRemoteView& rv) override {
        if (View* v = findView(rv.viewId())) releaseHeld(*v);
    }

    bool pointerLocked(layout::ElRemoteView& rv) override {
        View* v = findView(rv.viewId());
        return v && v->session && v->cursor.locked;
    }

private:
    static bool chordModsHeld(platform::KeyMods need, int mod) {
        namespace kmod = platform::kmod;
        for (platform::KeyMods m : {kmod::Ctrl, kmod::Alt, kmod::Shift, kmod::Gui}) {
            if ((need & m) && !(mod & m)) return false;
        }
        return true;
    }
};
Host g_hostImpl;

// ---- HTMLRemoteViewElement -------------------------------------------------

layout::ElRemoteView* control(Value self, bool create = true) {
    HostNodeState* st = hostNodeStateOfValue(self);
    dom::Element* el = st ? st->el : nullptr;
    if (!el) return nullptr;
    if (auto* rv = el->remoteViewControl()) return rv;
    if (!create || (el->tagName() != "remoteview" && el->tagName() != "REMOTEVIEW")) return nullptr;
    auto ctrl = std::make_unique<layout::ElRemoteView>();
    ctrl->setElement(el);
    el->setRemoteViewControl(std::move(ctrl));
    return el->remoteViewControl();
}

Value statsValue(View* v, layout::ElRemoteView& rv) {
    ObjectBuilder o;
    const char* path = "none";
    if (v && v->bridged) path = "gpu";
    else if (v && v->uploads) path = "cpu";
    o.set("path", ev::fromUtf8(path));
    // Every picture shown went decoder -> compositor on the GPU.
    o.set("zeroCopy", ev::fromBool(v && v->bridged > 0 && v->uploads == 0));
    o.set("pictures", ev::fromDouble(v ? double(v->pictures) : 0.0));
    o.set("gpuPictures", ev::fromDouble(v ? double(v->bridged) : 0.0));
    o.set("uploads", ev::fromDouble(v ? double(v->uploads) : 0.0));
    o.set("unshown", ev::fromDouble(v ? double(v->unshown) : 0.0));
    o.set("presented", ev::fromDouble(v ? double(v->presented) : 0.0));
    o.set("streamWidth", ev::fromDouble(double(rv.streamWidth())));
    o.set("streamHeight", ev::fromDouble(double(rv.streamHeight())));
    o.set("bridge", ev::fromBool(g.bridge != nullptr));
    o.set("bridgeImports", ev::fromDouble(g.bridge ? double(g.bridge->imports()) : 0.0));
    o.set("bridgeConversions", ev::fromDouble(g.bridge ? double(g.bridge->conversions()) : 0.0));
    if (!g.bridge && g.bridgeTried) o.set("bridgeError", ev::fromUtf8(g.bridgeError));
    return o.get();
}

}  // namespace

void decorateRemoteViewProto(ObjectBuilder& b) {
    // session: the bro.remote.connect() session shown here (null: none).
    b.accessor("session",
        [](Value self, std::span<const Value>) -> Value {
            layout::ElRemoteView* rv = control(self, false);
            View* v = rv ? findView(rv->viewId()) : nullptr;
            return v && v->sessionObject ? v->sessionObject->get() : ev::null();
        },
        [](Value self, std::span<const Value> a) -> Value {
            ev::Persistent selfRoot(self);
            ev::Persistent arg(argAt(a, 0));
            layout::ElRemoteView* rv = control(selfRoot.get());
            if (!rv) return ev::undefined();
            View& v = viewFor(rv->viewId());
            if (ev::isNull(arg.get()) || ev::isUndefined(arg.get())) {
                unbind(v);
                return ev::undefined();
            }
            broremote::ViewerSession* s = broremote::api::viewerSession(arg.get());
            if (!s) return ev::throwTypeError("remoteview.session: a bro.remote.connect() session, or null");
            if (v.session == s) return ev::undefined();
            // A session shows in one view: taken from any other.
            for (auto& [id, other] : g.views) {
                if (other.get() != &v && other->session == s) unbind(*other);
            }
            unbind(v);
            v.session = s;
            v.sessionObject = std::make_unique<ev::Persistent>(arg.get());
            return ev::undefined();
        });
    b.accessor("captured", [](Value self, std::span<const Value>) -> Value {
        layout::ElRemoteView* rv = control(self, false);
        return ev::fromBool(rv && rv->captured());
    }, nullptr);
    // capture(): focus the view and send it the keyboard.
    b.def("capture", 0, [](Value self, std::span<const Value>) -> Value {
        layout::ElRemoteView* rv = control(self);
        if (!rv || !rv->element()) return ev::undefined();
        hostFocusElement(rv->element());
        rv->setCaptured(true);
        return ev::undefined();
    });
    b.def("release", 0, [](Value self, std::span<const Value>) -> Value {
        if (layout::ElRemoteView* rv = control(self, false)) rv->setCaptured(false);
        return ev::undefined();
    });
    b.accessor("releaseChord",
        [](Value self, std::span<const Value>) -> Value {
            layout::ElRemoteView* rv = control(self);
            return ev::fromUtf8(rv ? viewFor(rv->viewId()).chordText : std::string(kDefaultReleaseChord));
        },
        [](Value self, std::span<const Value> a) -> Value {
            ev::Persistent selfRoot(self);
            const Value arg = argAt(a, 0);
            if (!ev::isString(arg)) return ev::throwTypeError("remoteview.releaseChord: a string like \"Ctrl+Alt+Escape\"");
            const std::string text = ev::toUtf8(arg);
            Chord c;
            if (!parseChord(text, c))
                return ev::throwTypeError("remoteview.releaseChord: modifiers and a key, like \"Ctrl+Alt+Escape\"");
            layout::ElRemoteView* rv = control(selfRoot.get());
            if (!rv) return ev::undefined();
            View& v = viewFor(rv->viewId());
            v.chordText = text;
            v.chord = c;
            return ev::undefined();
        });
    b.accessor("streamWidth", [](Value self, std::span<const Value>) -> Value {
        layout::ElRemoteView* rv = control(self, false);
        return ev::fromDouble(rv ? double(rv->streamWidth()) : 0.0);
    }, nullptr);
    b.accessor("streamHeight", [](Value self, std::span<const Value>) -> Value {
        layout::ElRemoteView* rv = control(self, false);
        return ev::fromDouble(rv ? double(rv->streamHeight()) : 0.0);
    }, nullptr);
    // stats(): how the pictures reach the screen.
    b.def("stats", 0, [](Value self, std::span<const Value>) -> Value {
        layout::ElRemoteView* rv = control(self);
        if (!rv) return ev::null();
        return statsValue(findView(rv->viewId()), *rv);
    });
}

void installRemoteViewHost(engine::Engine& engine) {
    g.engine = &engine;
    broremote::api::ViewerHooks hooks;
    hooks.sessionStarting = &sessionStarting;
    hooks.sessionGone = &sessionGone;
    broremote::api::setViewerHooks(std::move(hooks));
    engine.setRemoteViewHost(&g_hostImpl);
    engine.addFramePump(&pump);
    engine.addShutdownHook([] {
        for (auto& [id, v] : g.views) unbind(*v);
        g.views.clear();
        if (g.engine) g.engine->setRemoteViewHost(nullptr);
        g.bridge.reset();
    });
}

}  // namespace bro::bronze_host
