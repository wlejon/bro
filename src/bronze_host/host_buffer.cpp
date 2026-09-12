#include "bronze_host/host_globals_internal.h"
#include <api/api.h>

extern "C" void bronze_buffer_main();

namespace bro::bronze_host {

void installBufferGlobals() {
    bronze::embed::runEntry(bronze_buffer_main);
    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        Value buf = ev::getProperty(g.value, "Buffer");
        if (!ev::isUndefined(buf)) {
            ev::registerGlobal("Buffer", buf);
        }
    }
}

} // namespace bro::bronze_host
