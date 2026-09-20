// bro-native-manifest: prints the native manifest bro's own JavaScript
// (js/bro_core.js) is compiled against, the same way bronze's own test suite
// does (bronze/tests/native/manifest_tool.cpp). It links the binding layer
// and calls the SAME registerBroNatives bro calls at run time, then writes
// what the registry holds through embed::writeNativeManifest. Run at BUILD
// time by CMakeLists.txt; the manifest is an output of the host, never a
// file a person maintains beside it.
//
//   bro-native-manifest <out.json>
//
// No engine is constructed: registration records C entry points and
// signatures, and nothing here calls one.

#include "bronze_host/host_natives.h"
#include "embed/embed.h"

#include <cstdio>
#include <exception>
#include <string>

namespace {
struct InitProbe {
    InitProbe() {
        std::set_terminate([]() {
            try {
                auto e = std::current_exception();
                if (e) std::rethrow_exception(e);
            } catch (const std::exception& exc) {
                std::fprintf(stderr, "bro-native-manifest uncaught exception: %s\n", exc.what());
            } catch (...) {
                std::fprintf(stderr, "bro-native-manifest unknown uncaught exception\n");
            }
            std::fflush(stderr);
            std::abort();
        });
    }
} g_probe;
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: bro-native-manifest <out.json>\n");
        return 2;
    }
    try {
        std::string err;
        if (!bro::bronze_host::registerBroNatives(&err)) {
            std::fprintf(stderr, "bro-native-manifest: %s\n", err.c_str());
            return 1;
        }
        if (!bronze::embed::writeNativeManifest(argv[1], &err)) {
            std::fprintf(stderr, "bro-native-manifest: %s\n", err.c_str());
            return 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "bro-native-manifest exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "bro-native-manifest unknown exception\n");
        return 1;
    }
    return 0;
}
