#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bro::table {

/// Compresses `src[0..srcLen)` into raw LZ4 block format.
bool lz4BlockCompress(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out);

/// Decompresses raw LZ4 block `src[0..srcLen)` into pre-sized `dst[0..expectedDstLen)`.
bool lz4BlockDecompress(const uint8_t* src, size_t srcLen, uint8_t* dst,
                        size_t expectedDstLen, std::string* err = nullptr);

/// Decompresses raw Snappy block `src[0..srcLen)` (with leading varint uncompressed length) into `out`.
bool snappyDecompress(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out,
                      std::string* err = nullptr);

}  // namespace bro::table
