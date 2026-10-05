#pragma once

#include "bronze_host/host_class.h"

namespace bro::engine { class Engine; }

namespace bro::bronze_host {

void decorateTemplateProto(ObjectBuilder& b);

// HTMLDialogElement's members (host_dialog.cpp).
void decorateDialogProto(ObjectBuilder& b);
// The engine-side hooks a modal dialog needs: Escape's close request.
void installDialogHooks(engine::Engine& engine);

}  // namespace bro::bronze_host
