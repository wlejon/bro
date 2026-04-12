#pragma once

extern "C" {
#include "quickjs.h"
}

namespace bro::net { class NetworkManager; }

namespace bro::js {

class NetBindings {
public:
    static void install(JSContext* ctx, net::NetworkManager* mgr);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
