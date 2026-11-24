#pragma once

#include "table/table.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::table {

struct SaveOptions {
    bool compress = false;  // When true, writes `.brotable` v4 with per-column LZ4 compression
};

/// Serializes an immutable TableSnapshot into `.brotable` v3/v4 binary format.
std::vector<uint8_t> serializeSnapshotBytes(const TableSnapshot& snap, const SaveOptions& opts);
inline std::vector<uint8_t> serializeSnapshotBytes(const TableSnapshot& snap) {
    return serializeSnapshotBytes(snap, SaveOptions{});
}

/// Validates and copies `.brotable` v1/v2/v3/v4 bytes into a fresh mutable Table (restoring indexes).
std::shared_ptr<Table> deserializeTableBytes(const uint8_t* data, size_t size,
                                             std::string* outError = nullptr);

/// Streams a `.brotable` v3/v4 snapshot directly to disk atomically with fixed 4 MiB buffering.
bool saveSnapshotToFile(const TableSnapshot& snap, const std::string& path,
                        const SaveOptions& opts, std::string* outError = nullptr);
inline bool saveSnapshotToFile(const TableSnapshot& snap, const std::string& path,
                               std::string* outError = nullptr) {
    return saveSnapshotToFile(snap, path, SaveOptions{}, outError);
}

/// Loads a `.brotable` v1/v2/v3/v4 file into a mutable Table (restoring persisted indexes).
std::shared_ptr<Table> loadTableFromFile(const std::string& path,
                                         std::string* outError = nullptr);

/// Maps a `.brotable` v1/v2/v3 file read-only via OS mmap/MapViewOfFile and returns a zero-copy
/// TableSnapshot whose ColumnChunks and v3 secondary indexes point directly into mapped pages.
/// Rejects compressed v4 files (use loadTableFromFile instead).
std::shared_ptr<const TableSnapshot> mmapSnapshotFromFile(const std::string& path,
                                                          std::string* outError = nullptr,
                                                          bool verifyChecksum = false);

}  // namespace bro::table
