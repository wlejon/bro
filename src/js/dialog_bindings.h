#pragma once

extern "C" {
#include "quickjs.h"
}

struct SDL_Window;

namespace bro::js {

class DialogBindings {
public:
    /// Install showOpenFileDialog() and showSaveFileDialog() on the global object.
    static void install(JSContext* ctx, SDL_Window* window);
};

} // namespace bro::js
