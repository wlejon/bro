#pragma once

#include "table/table.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::table {

/// Serializes an immutable TableSnapshot into `.brotable` v3 binary format (64-byte aligned columns & mapped indexes + streaming checksums).
std::vector<uint8_t> serializeSnapshotBytes(const TableSnapshot& snap);

/// Validates and copies `.brotable` v1/v2/v3 bytes into a fresh mutable Table (restoring v2/v3 indexes).
std::shared_ptr<Table> deserializeTableBytes(const uint8_t* data, size_t size,
                                             std::string* outError = nullptr);

/// Streams a `.brotable` v3 snapshot directly to disk atomically with fixed 4 MiB buffering.
bool saveSnapshotToFile(const TableSnapshot& snap, const std::string& path,
                        std::string* outError = nullptr);

/// Loads a `.brotable` v1/v2/v3 file into a mutable Table (restoring persisted v2/v3 indexes).
std::shared_ptr<Table> loadTableFromFile(const std::string& path,
                                         std::string* outError = nullptr);

/// Maps a `.brotable` v1/v2/v3 file read-only via OS mmap/MapViewOfFile and returns a zero-copy
/// TableSnapshot whose ColumnChunks and v3 secondary indexes point directly into mapped pages.
std::shared_ptr<const TableSnapshot> mmapSnapshotFromFile(const std::string& path,
                                                          std::string* outError = nullptr,
                                                          bool verifyChecksum = false);

}  // namespace bro::table
