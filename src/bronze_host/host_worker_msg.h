#pragma once

#include "bronze_host/host_globals_internal.h"
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace bro::bronze_host {

struct SerializedImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

struct Message {
    std::vector<uint8_t> data;
    std::vector<std::vector<uint8_t>> transferredBuffers;
    std::vector<SerializedImage> transferredImages;
};

bool serializeMessage(Value val, std::span<const Value> transfers, Message& out);
Value deserializeMessage(const Message& msg, size_t offset = 0);

} // namespace bro::bronze_host
