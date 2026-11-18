#pragma once

// URL-valued reflected attributes and HTMLHyperlinkElementUtils
// (host_url_reflect.cpp).

#include "bronze_host/host_builder.h"

#include <string>

namespace bro::dom {
class Element;
}  // namespace bro::dom

namespace bro::bronze_host {

// The attribute `attr` of `el` as a URL: resolved against the document's base
// URL (a <base>'s href against the document's URL), "" without the attribute.
std::string reflectedUrl(const dom::Element* el, const char* attr);
// protocol / host / hostname / port / pathname / search / hash / origin /
// username / password and toString(), on HTMLAnchorElement and
// HTMLAreaElement.
void decorateHyperlinkProto(ObjectBuilder& b);
// relList: a DOMTokenList over `rel` (a, area, link).
void decorateRelList(ObjectBuilder& b);

}  // namespace bro::bronze_host
