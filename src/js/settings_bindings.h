#pragma once

struct JSContext;

namespace bro::engine { class Settings; class Engine; }
namespace bro::platform { class Window; }

namespace bro::js {

class SettingsBindings {
public:
    /// `engine` powers the polled action-state surface
    /// (getActionStrength / isActionPressed); null degrades those to 0/false.
    static void install(JSContext* ctx, engine::Settings* settings,
                        platform::Window* window,
                        engine::Engine* engine = nullptr);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
