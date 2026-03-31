#pragma once

#include "layout/draw_traversal.h"
#include "layout/skia_text_metrics.h"
#include <memory>
#include <string>
#include <vector>

namespace bro::js { class Runtime; class Timers; }
namespace bro::dom { class Document; class Element; class Event; }
namespace bro::layout { class DrawTraversal; }
namespace bro::render { class Renderer; }
namespace bro::canvas { class CanvasScene; }
namespace bro::engine { class SystemOverlay; }

namespace bro::headless {

/// Headless engine for automated testing and debugging.
/// No window, no GPU — just DOM + JS + layout.
/// Accepts text commands and outputs DOM state/diffs.
class Headless {
public:
    explicit Headless(const std::string& appDir, int width = 1024, int height = 768);
    ~Headless();

    /// Dump the full DOM as HTML to stdout.
    std::string dumpHTML() const;

    /// Dump a single element's outer HTML.
    std::string dumpElement(const std::string& selector) const;

    /// Simulate a click on the element matching the CSS selector.
    /// Returns true if an element was found and clicked.
    bool click(const std::string& selector);

    /// Set a text value on an input element (future use).
    bool setValue(const std::string& selector, const std::string& value);

    /// Evaluate arbitrary JS and return the string result.
    std::string eval(const std::string& code);

    /// Tick timers by the given number of milliseconds.
    void advanceTime(double ms);

    /// Run pending JS jobs (promises, microtasks).
    void flush();

    /// Run an interactive command loop reading from stdin.
    void runInteractive();

    /// Run a script file with commands.
    void runScript(const std::string& path);

    /// Render the current page to a PNG file.
    bool screenshot(const std::string& path);

    /// Process a single command line. Returns false on "quit"/"exit".
    bool processCommand(const std::string& line);

    dom::Document* document() const { return document_.get(); }

private:
    dom::Element* querySelector(const std::string& selector) const;
    void dispatchClickOn(dom::Element* target);

    std::unique_ptr<bro::render::Renderer> renderer_;
    std::unique_ptr<bro::js::Runtime> jsRuntime_;
    std::unique_ptr<bro::js::Timers> timers_;
    std::unique_ptr<bro::dom::Document> document_;
    std::unique_ptr<layout::DrawTraversal> drawTraversal_;
    std::unique_ptr<layout::HeadlessTextMetrics> textMetrics_;
    layout::FontManager fontManager_;

    int viewportWidth_;
    int viewportHeight_;
    double virtualTime_ = 0.0;
    std::string lastDump_; // for diff
    std::unique_ptr<canvas::CanvasScene> canvasScene_;
    canvas::CanvasScene* canvasScenePtr_ = nullptr; // raw ptr for JS
    std::unique_ptr<engine::SystemOverlay> systemOverlay_;
};

} // namespace bro::headless
