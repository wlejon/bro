#pragma once

#include <qjsbind/qjsbind.h>

#include <string>

namespace bro::js {

/// Register the VideoEncoder class on the global object.
/// `basePath` is the app directory used to resolve relative output paths,
/// matching how Image and fetch resolve relative inputs.
class VideoBindings {
public:
    static void install(JSContext* ctx, const std::string& basePath);
};

} // namespace bro::js
