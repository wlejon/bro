#pragma once

#include <qjsbind/qjsbind.h>

namespace bro::engine { class Engine; }

namespace bro::js {

class ServerBindings {
public:
    static void install(JSContext* ctx, engine::Engine* engine);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
