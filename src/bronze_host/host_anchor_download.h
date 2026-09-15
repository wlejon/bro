#pragma once

#include <string>

namespace bro::dom {
class Element;
}

namespace bro::bronze_host {

bool runAnchorDownload(dom::Element* el);

const std::string& lastDownloadPath();
void setLastDownloadPath(std::string p);

} // namespace bro::bronze_host
