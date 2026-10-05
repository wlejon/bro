#pragma once

#include "bronze_host/host_builder.h"
#include <string>

namespace bro::dom { class Document; }

namespace bro::bronze_host {

Value makeHostMatchMediaObject(const std::string& rawQuery);
void deliverHostMediaQueryChanges();
void removeHostMediaQueriesForDocument(dom::Document* doc);
void clearHostMediaQueries();

} // namespace bro::bronze_host
