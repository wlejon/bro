#pragma once

#include <qjsbind/qjsbind.h>

namespace bro::js {

/// Install bro.image.gpu — WebGL2-backed colormap helpers.
///
/// Must be called after WebGL2Bindings::install (which exposes the WebGL2
/// API the helper depends on) and after brokit::api::installAll (which
/// installs the bro.image namespace via brokit; this binding extends it
/// with `.gpu`).
class ImageGPUBindings {
public:
    static void install(JSContext* ctx);
};

} // namespace bro::js
