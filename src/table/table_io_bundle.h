#pragma once

#include "table/table_io.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bro::table {

struct BundleEntryInput {
    std::string name;
    std::shared_ptr<const TableSnapshot> snapshot;
};

struct LoadedBundleEntry {
    std::string name;
    std::shared_ptr<Table> table;
};

struct MappedBundleEntry {
    std::string name;
    std::shared_ptr<const TableSnapshot> snapshot;
};

/// Saves multiple named TableSnapshots into a single 4096B-aligned `.brotable` bundle atomically.
bool saveBundleToFile(const std::vector<BundleEntryInput>& entries,
                      const std::string& path,
                      const SaveOptions& opts,
                      std::string* outError = nullptr);

inline bool saveBundleToFile(const std::vector<BundleEntryInput>& entries,
                             const std::string& path,
                             std::string* outError = nullptr) {
    return saveBundleToFile(entries, path, SaveOptions{}, outError);
}

/// Loads all tables from a `.brotable` bundle (supports both uncompressed v3 and LZ4 v4 members).
bool loadBundleFromFile(const std::string& path,
                        std::vector<LoadedBundleEntry>& outEntries,
                        std::string* outError = nullptr);

/// Maps a `.brotable` bundle read-only with one OS mapping shared across all member snapshots.
bool mmapBundleFromFile(const std::string& path,
                        std::vector<MappedBundleEntry>& outEntries,
                        std::string* outError = nullptr,
                        bool verifyChecksum = false);

}  // namespace bro::table
