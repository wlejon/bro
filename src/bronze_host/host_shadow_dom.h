#pragma once

#include "bronze_host/host_class.h"

namespace bro::dom {
class ShadowRoot;
}

namespace bro::bronze_host {

const HostClass& shadowRootHostClass();
void installShadowRootClass();
Value hostShadowRootValue(dom::ShadowRoot* sr);
void decorateElementShadow(ObjectBuilder& b);

}  // namespace bro::bronze_host
