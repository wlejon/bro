#pragma once

#include <string>

struct JSContext;

namespace bro::js {

class FetchBindings {
public:
    /// Register fetch(), Headers, Request, Response, TextDecoder on the global object.
    /// basePath is the app directory used to resolve relative URLs to local files.
    static void install(JSContext* ctx, const std::string& basePath);
};

} // namespace bro::js
