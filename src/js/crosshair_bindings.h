#pragma once

#include <qjsbind/qjsbind.h>

namespace bro::engine { class Engine; }

namespace bro::js {

class CrosshairBindings {
public:
    static void install(JSContext* ctx, engine::Engine* engine);
};

} // namespace bro::js
