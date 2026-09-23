#pragma once

#include "bronze_host/host_internal.h"

namespace bro::bronze_host {

void decorateTemplateProto(ObjectBuilder& b);

// HTMLDialogElement's members (host_dialog.cpp).
void decorateDialogProto(ObjectBuilder& b);

}  // namespace bro::bronze_host
