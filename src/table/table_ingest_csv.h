#pragma once

#include "table/table.h"

#include <memory>
#include <string>
#include <vector>

namespace bro::table {

struct CsvIngestOptions {
    char delimiter = ',';
    char quote = '"';
    bool hasHeader = true;
    size_t skipRows = 0;
    size_t maxRows = 0;              // 0 = unlimited
    size_t inferSampleRows = 2048;
    size_t dictMaxCardinality = 4096;
    std::vector<ColumnDef> explicitSchema;
};

/// Parses CSV/TSV bytes into a bro.table Table (schema inferred or explicit).
std::shared_ptr<Table> ingestCsvFromMemory(const char* data, size_t size,
                                           const CsvIngestOptions& opts = {},
                                           std::string* outError = nullptr);

/// Streams CSV/TSV from disk in fixed 1 MiB read chunks without loading whole file into RAM.
std::shared_ptr<Table> ingestCsvFromFile(const std::string& path,
                                         const CsvIngestOptions& opts = {},
                                         std::string* outError = nullptr);

}  // namespace bro::table
