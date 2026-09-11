#include "engine/headless_driver.h"
#include "engine/engine.h"
#include "dom/document.h"
#include "dom/element.h"
#include "dom/event.h"
#if BRO_WITH_BRONZE
#include "bronze_host/app_module.h"
#include "bronze_host/gl_profile.h"
#endif

int main(int argc, char* argv[]) {
    bro::engine::HeadlessHooks hooks;
#if BRO_WITH_BRONZE
    hooks.providesCompiledApp = [](const std::string& appDir) {
        return bro::bronze_host::findAppModule(appDir).has_value();
    };
    hooks.afterEngine = [](bro::engine::Engine& engine) {
        if (auto modulePath = bro::bronze_host::findAppModule(engine.appDir()))
            bro::bronze_host::runAppModule(engine, *modulePath);
    };
    hooks.beforeExit = [] { bro::bronze_host::hostProfileDump(); };
#endif
    return bro::engine::runHeadless(argc, argv, hooks);
}
