// brokit's Node-flavoured surface for the bronze realm: `require()` and the
// modules it hands back (`fs`, `path`, `os`, `child_process`), plus the
// `process` global. bro apps have always had these — the launcher and the
// project manager read the workshop with `fs` and spawn tools with
// `child_process` — and brokit is where they are implemented. This file only
// installs them into the realm this layer owns and tells brokit's fs where
// bro's virtual paths (`/app`, `/lib`, `/system`, ...) point, so that
// `fs.readFileSync('/app/data.json')` reads the file `fetch('/app/data.json')`
// reads.
//
// Deliberately the Node half of brokit and nothing else: its web half
// (timers, URL, fetch, Blob, TextEncoder, ...) is answered by this layer's
// own host_*.cpp files, which are bound to the engine's clock and document,
// and a second registration of those names would replace the bound one with
// one that is not.

#include "bronze_host/bronze_host.h"
#include "bronze_host/host_internal.h"

#include "engine/engine.h"
#include "util/asset_mounts.h"

#include "api/api.h"

namespace bro::bronze_host {

void installNodeCoreGlobals(engine::Engine& engine) {
    namespace bk = brokit::api;

    bk::installModuleRegistry();
    bk::installProcess();
    bk::installOS();
    bk::installPath();
    bk::installFS();
    bk::installChildProcess();
    bk::installRequire();
    bk::installBuffer();

    bk::installConsole();
    bk::installCrypto();
    bk::installSubtleCrypto();
    bk::installEventTarget();
    bk::installMessageChannel();
    bk::installReadableStream();
    bk::installWritableStream();
    bk::installCompression();

    for (const auto& [prefix, target] : engine.assetMounts().mounts()) {
        bk::addFsPrefixMount(prefix, target);
    }
    if (!engine.appDir().empty()) {
        bk::addFsBasePath(engine.appDir());
    }
}

}  // namespace bro::bronze_host
