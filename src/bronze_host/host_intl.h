#pragma once

namespace bro::engine {
class Engine;
}

namespace bro::bronze_host {

/// Installs ECMA-402 Intl namespace and prototype hooks on globalThis.
void installIntlGlobals();

}  // namespace bro::bronze_host
