// bro's own JavaScript, shipped as bronze-compiled objects.
//
// Each module under js/ is compiled at build time by bro_compile_js
// (CMakeLists.txt) into an object with its own entry symbol and linked into
// this library, the way brokit ships its polyfills (../brokit/src/api). An
// installer here enters it through bronze::embed::runEntry — the module's top
// level runs in the realm the host has built so far — and then LIFTS the
// names it assigned onto `globalThis` into bronze's host-global registry.
// The lift is what makes them compiled-app globals: registeredHostGlobals()
// reads the registry back as the manifest every app is compiled against, and
// `bronze_global_get` asks the registry before the global object, so without
// it a compiled read of `MutationObserver` would miss a class that is right
// there on globalThis.
//
// Nothing here probes the filesystem for a .js at run time. The modules are
// in the binary.

#include "bronze_host/host_internal.h"
#include "embed/embed.h"

extern "C" void bro_observers_main();
extern "C" void bro_net_sync_main();
extern "C" void bro_image_gpu_main();

namespace bro::bronze_host {

namespace {

void adoptGlobalProperty(const char* name) {
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return;
    Value v = ev::getProperty(gt.value, name);
    if (!ev::isUndefined(v)) ev::registerGlobal(name, v);
}

}  // namespace

void installObserversModule() {
    // The module reads `__bro_observers` at its top level, so the hook object
    // is registered first (host_observer_hooks.cpp).
    installObserverHooks();
    bronze::embed::runEntry(bro_observers_main);
    adoptGlobalProperty("MutationObserver");
    adoptGlobalProperty("MutationRecord");
    adoptGlobalProperty("ResizeObserver");
    adoptGlobalProperty("ResizeObserverEntry");
    adoptGlobalProperty("ResizeObserverSize");
    adoptGlobalProperty("IntersectionObserver");
    adoptGlobalProperty("IntersectionObserverEntry");
}

void installNetSyncModule() {
    bronze::embed::runEntry(bro_net_sync_main);
    adoptGlobalProperty("__bro_net_sync");
}

void installImageGpuModule() {
    bronze::embed::runEntry(bro_image_gpu_main);
    adoptGlobalProperty("__bro_image_gpu");
}

}  // namespace bro::bronze_host
