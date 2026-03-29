#pragma once

#include <litehtml.h>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "quickjs.h"
}

namespace bro::render { class Renderer; }
namespace bro::layout { class BroContainer; }
namespace bro::dom { class Document; }
namespace bro::js { class Timers; }

namespace bro::engine {

class SystemOverlay {
public:
    SystemOverlay(render::Renderer* renderer, int vpW, int vpH);
    ~SystemOverlay();

    /// Scan systemDir for subdirectories containing index.html and load each as a panel.
    void loadPanels(const std::string& systemDir);

    /// Toggle overlay visibility.
    void toggle();
    bool isVisible() const { return visible_; }

    /// Update performance data (called each stats accumulation cycle).
    void updatePerf(double fps, double frameTime, double js, double layout,
                    double raster, double gpu, double draw, int vpW, int vpH);

    /// Tick JS timers and run pending jobs.
    void tick(double nowMs);

    /// Render all visible panels to the renderer (call between beginFrame/endFrame).
    void render(int vpW, int vpH);

    /// Handle viewport resize.
    void onResize(int w, int h);

    struct Panel {
        std::string name;
        litehtml::document::ptr litehtmlDoc;
        std::unique_ptr<layout::BroContainer> container;
        std::unique_ptr<dom::Document> document;
    };

private:
    void installMinimalBindings();
    void installBroObject();

    render::Renderer* renderer_;
    int viewportWidth_;
    int viewportHeight_;
    bool visible_ = false;

    // Own JS environment (isolated from the app)
    JSRuntime* jsRt_ = nullptr;
    JSContext* jsCtx_ = nullptr;
    std::unique_ptr<js::Timers> timers_;

    // __bro.perf JS object references (for fast property updates)
    JSValue broPerfObj_ = JS_UNDEFINED;

    std::vector<Panel> panels_;
};

} // namespace bro::engine
