#include "bronze_host/gl_internal.h"
#include "bronze_host/host_internal.h"
#include "embed/embed.h"

namespace bro::bronze_host {

Value makeBroNetValue() {
    ObjectBuilder b;
    return b.get();
}

void installNetGlobals() {
}

void drainNetEvents() {
}

}  // namespace bro::bronze_host
