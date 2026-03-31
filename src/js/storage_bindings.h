#pragma once

#include <string>

extern "C" {
#include "quickjs.h"
}

namespace bro::js {

class StorageBindings {
public:
    /// Register `localStorage` on the global object.
    /// storagePath is the file path for persisting data (e.g. appDir/.storage.json).
    static void install(JSContext* ctx, const std::string& storagePath);
    static void installSessionStorage(JSContext* ctx);
    static void cleanup(JSContext* ctx);
};

} // namespace bro::js
