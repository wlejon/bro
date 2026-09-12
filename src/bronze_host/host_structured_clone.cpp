#include "bronze_host/host_globals_internal.h"
#include <api/api.h>

extern "C" void bronze_structuredclone_main();

namespace bro::bronze_host {

void installStructuredCloneGlobals() {
    bronze::embed::runEntry(bronze_structuredclone_main);
    auto g = ev::globalValue("globalThis");
    if (g.found && ev::isObject(g.value)) {
        Value sc = ev::getProperty(g.value, "structuredClone");
        if (!ev::isUndefined(sc)) {
            ev::registerGlobal("structuredClone", sc);
        }
    }
}

} // namespace bro::bronze_host
