#pragma once

struct JSContext;

namespace bro::js {

class ImageBindings {
public:
    /// Register the Image/HTMLImageElement constructor on the global object.
    static void install(JSContext* ctx);
};

} // namespace bro::js
