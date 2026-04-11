#pragma once

struct JSContext;

namespace bro::engine { class Settings; }
namespace bro::platform { class Window; }

namespace bro::js {

class SettingsBindings {
public:
    static void install(JSContext* ctx, engine::Settings* settings,
                        platform::Window* window);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
