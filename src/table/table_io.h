#pragma once

#include "table/table.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::table {

/// Serializes an immutable TableSnapshot into `.brotable` v2 binary format (64-byte aligned + FNV-1a checksum + index specs + null bitmaps).
std::vector<uint8_t> serializeSnapshotBytes(const TableSnapshot& snap);

/// Validates and copies `.brotable` v1/v2 bytes into a fresh mutable Table (restoring persisted v2 indexes).
std::shared_ptr<Table> deserializeTableBytes(const uint8_t* data, size_t size,
                                             std::string* outError = nullptr);

/// Writes a `.brotable` v2 snapshot to disk atomically (returns false and populates outError on failure).
bool saveSnapshotToFile(const TableSnapshot& snap, const std::string& path,
                        std::string* outError = nullptr);

/// Loads a `.brotable` v1/v2 file into a mutable Table (restoring persisted v2 indexes).
std::shared_ptr<Table> loadTableFromFile(const std::string& path,
                                         std::string* outError = nullptr);

/// Maps a `.brotable` v1/v2 file read-only via OS mmap/MapViewOfFile and returns a zero-copy
/// TableSnapshot whose ColumnChunks (including null bitmaps) point directly into mapped pages.
std::shared_ptr<const TableSnapshot> mmapSnapshotFromFile(const std::string& path,
                                                          std::string* outError = nullptr);

}  // namespace bro::table
