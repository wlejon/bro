#pragma once

// ElRemoteView: the replaced-element controller behind <remoteview>, a remote
// screen (a bro.remote.connect() session) shown as its own compositor layer.
//
// This is only the element's side: a process-unique id the layer break names
// (render::RemoteViewLayerSource), the stream size as the intrinsic size,
// and the capture state. The session, its pictures and its input belong to
// the engine's RemoteViewHost (bronze_host/host_remote_view.cpp, in a build
// with BRO_WITH_REMOTE), which finds the controller by id. Main thread
// only, except getContentSize() (layout's thread), which reads two atomics.

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace bro::dom { class Element; }

namespace bro::layout {

class ElRemoteView {
public:
    ElRemoteView();
    ~ElRemoteView();
    ElRemoteView(const ElRemoteView&) = delete;
    ElRemoteView& operator=(const ElRemoteView&) = delete;

    void setElement(dom::Element* el) { elem_ = el; }
    dom::Element* element() const { return elem_; }

    // Process-unique, never recycled.
    uint64_t viewId() const { return viewId_; }
    static ElRemoteView* byId(uint64_t id);
    static void forEach(const std::function<void(ElRemoteView&)>& fn);

    // The stream's size, as the intrinsic size (300 x 150 until a stream is
    // known, as for <canvas>). True when it changed.
    bool setStreamSize(uint32_t w, uint32_t h);
    void getContentSize(float& w, float& h) const;
    uint32_t streamWidth() const { return streamW_.load(std::memory_order_relaxed); }
    uint32_t streamHeight() const { return streamH_.load(std::memory_order_relaxed); }

    // Keyboard and pointer go to the remote screen, raw, while captured (and
    // the element focused): engine/input_remote.cpp.
    bool captured() const { return captured_; }
    void setCaptured(bool on) { captured_ = on; }

    // The pointer shape over the view: the remote screen's (a CSS cursor
    // name; "none" while it hides its pointer). Empty: the element's own.
    const std::string& cursorCss() const { return cursorCss_; }
    void setCursorCss(std::string css) { cursorCss_ = std::move(css); }

private:
    std::string cursorCss_;
    dom::Element* elem_ = nullptr;
    uint64_t viewId_ = 0;
    std::atomic<uint32_t> streamW_{0}, streamH_{0};
    bool captured_ = false;
};

}  // namespace bro::layout
