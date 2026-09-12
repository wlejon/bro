#pragma once

#include "bronze_host/gl_internal.h"
#include <string>

namespace bro::dom { class Document; }

namespace bro::bronze_host {

Value makeHostMatchMediaObject(const std::string& rawQuery);
void deliverHostMediaQueryChanges();
void removeHostMediaQueriesForDocument(dom::Document* doc);

} // namespace bro::bronze_host
