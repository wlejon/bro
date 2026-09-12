#pragma once

#include "embed/embed.h"

namespace bro::dom { class Element; }

namespace bro::bronze_host {

bronze::embed::Value makeCanvas2DContextValue(bronze::embed::Value canvasVal, dom::Element* el = nullptr);

}  // namespace bro::bronze_host
