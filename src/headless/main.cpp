#include "engine/headless_driver.h"
#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#include "bronze_host/app_module.h"
#include "bronze_host/gl_profile.h"
#include "bronze_host/host_storage.h"
#include "util/crash_handler.h"

int main(int argc, char* argv[]) {
    // First thing, before anything can print or fault: unbuffer the standard
    // streams and arm the crash handlers. A headless run is usually redirected
    // to a file, which is exactly when the CRT starts buffering, and a run
    // that renders for an hour and then segfaults used to discard every line
    // it had written along with the reason it died.
    bro::util::installCrashHandler();

    bro::engine::HeadlessHooks hooks;
    hooks.providesCompiledApp = [](const std::string& appDir) {
        return bro::bronze_host::findAppModule(appDir).has_value();
    };
    hooks.afterEngine = [](bro::engine::Engine& engine) {
        if (auto modulePath = bro::bronze_host::findAppModule(engine.appDir()))
            bro::bronze_host::runAppModule(engine, *modulePath);
    };
    hooks.beforeExit = [] {
        bro::bronze_host::flushHostStorage();
        bro::bronze_host::hostProfileDump();
    };
    return bro::engine::runHeadless(argc, argv, hooks);
}
