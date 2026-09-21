#pragma once

#include <string>

namespace bro::bronze_host {

void reloadHostStorage(const std::string& basePath);
void flushHostStorage();

}  // namespace bro::bronze_host
