#pragma once

#include "table/table.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::table {

/// Serializes an immutable TableSnapshot into `.brotable` binary format (64-byte aligned payloads).
std::vector<uint8_t> serializeSnapshotBytes(const TableSnapshot& snap);

/// Validates and copies `.brotable` bytes into a fresh mutable Table.
std::shared_ptr<Table> deserializeTableBytes(const uint8_t* data, size_t size,
                                             std::string* outError = nullptr);

/// Writes a `.brotable` snapshot to disk atomically (returns false and populates outError on failure).
bool saveSnapshotToFile(const TableSnapshot& snap, const std::string& path,
                        std::string* outError = nullptr);

/// Loads a `.brotable` file into a mutable Table.
std::shared_ptr<Table> loadTableFromFile(const std::string& path,
                                         std::string* outError = nullptr);

/// Maps a `.brotable` file read-only via OS mmap/MapViewOfFile and returns a zero-copy
/// TableSnapshot whose ColumnChunks point directly into the mapped pages.
std::shared_ptr<const TableSnapshot> mmapSnapshotFromFile(const std::string& path,
                                                          std::string* outError = nullptr);

}  // namespace bro::table
