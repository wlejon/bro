#pragma once

#include "embed/embed.h"
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace bro::bronze_host {

void registerHostCalleeName(bronze::Value fnVal, std::string_view name);
bool hostCalleeNamer(uint64_t calleeBits, void* code, char* out, size_t outSize);
void initHostCalleeNamer();

}  // namespace bro::bronze_host
