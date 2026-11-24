#pragma once

#include "table/table.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bro::table {

struct ParquetIngestOptions {
    std::vector<std::string> columns;  // empty = all leaf columns
    size_t maxRows = 0;                // 0 = unlimited
    bool stringAsDict = false;
};

struct ParquetSaveOptions {
    bool compressLz4 = false;          // LZ4_RAW page compression when true
    uint32_t rowGroupRows = 65536;
};

/// Reads an Apache Parquet file from an in-memory buffer into a bro.table Table.
std::shared_ptr<Table> ingestParquetFromMemory(const uint8_t* data, size_t size,
                                               const ParquetIngestOptions& opts = {},
                                               std::string* outError = nullptr);

/// Streams/maps an Apache Parquet file from disk into a bro.table Table.
std::shared_ptr<Table> ingestParquetFromFile(const std::string& path,
                                             const ParquetIngestOptions& opts = {},
                                             std::string* outError = nullptr);

/// Serializes an immutable TableSnapshot into Apache Parquet format bytes.
std::vector<uint8_t> serializeSnapshotToParquetBytes(const TableSnapshot& snap,
                                                     const ParquetSaveOptions& opts = {},
                                                     std::string* outError = nullptr);

/// Writes an immutable TableSnapshot to disk as an Apache Parquet file.
bool saveSnapshotToParquetFile(const TableSnapshot& snap, const std::string& path,
                               const ParquetSaveOptions& opts = {},
                               std::string* outError = nullptr);

}  // namespace bro::table
