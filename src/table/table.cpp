#include "table/table.h"

#include <algorithm>
#include <cstring>

namespace bro::table {

namespace {

double readNumericCell(const TableChunk& ch, size_t colIdx, uint32_t row) {
    ScalarValue sv = ch.columns[colIdx]->readCell(row, nullptr);
    if (colTypeIsFloat(sv.type)) return sv.f64;
    if (colTypeIsUnsignedInt(sv.type)) return static_cast<double>(sv.u64);
    return static_cast<double>(sv.i64);
}

bool spatialUsesColumn(const SpatialIndex& si, size_t colIdx) {
    for (uint8_t d = 0; d < si.dims(); ++d) {
        if (si.cols()[d] == colIdx) return true;
    }
    return false;
}

bool readSpatialCoords(const TableChunk& ch, const SpatialIndex& si, uint32_t row,
                       double& x, double& y, double& z) {
    const auto& cols = si.cols();
    if (ch.columns[cols[0]]->isNull(row) || ch.columns[cols[1]]->isNull(row)) return false;
    if (si.dims() >= 3 && ch.columns[cols[2]]->isNull(row)) return false;
    x = readNumericCell(ch, cols[0], row);
    y = readNumericCell(ch, cols[1], row);
    z = (si.dims() >= 3) ? readNumericCell(ch, cols[2], row) : 0.0;
    return true;
}

template <typename ChunkContainer>
TableStats computeStatsCommon(const Schema& schema, const StringDictionary* dict,
                              const ChunkContainer& chunks, uint64_t totalAliveRows,
                              size_t hashCount, size_t rangeCount, size_t spatialCount) {
    TableStats st;
    st.rowCount = static_cast<size_t>(totalAliveRows);
    st.chunkCount = chunks.size();
    st.columnCount = schema.size();
    st.dictEntries = dict ? dict->size() : 0;
    st.hashIndexes = hashCount;
    st.rangeIndexes = rangeCount;
    st.spatialIndexes = spatialCount;
    for (const auto& ch : chunks) {
        st.allocatedRows += ch.rowCount;
        st.tombstoneCount += (ch.rowCount >= ch.aliveCount) ? (ch.rowCount - ch.aliveCount) : 0;
        for (size_t c = 0; c < schema.size() && c < ch.columns.size(); ++c) {
            st.fixedBytes += static_cast<size_t>(ch.rowCount) * colTypeElemSize(schema.columns[c].type);
            if (ch.columns[c]) {
                st.stringArenaBytes += ch.columns[c]->stringArenaSize();
                st.nullBitmapBytes += ch.columns[c]->nullBitmapBytes();
            }
        }
    }
    st.tombstoneRatio = (st.allocatedRows > 0)
                            ? static_cast<double>(st.tombstoneCount) / static_cast<double>(st.allocatedRows)
                            : 0.0;
    return st;
}

}  // namespace

bool Schema::addColumn(std::string name, ColType type) {
    if (nameToIndex.find(name) != nameToIndex.end()) return false;
    const size_t idx = columns.size();
    nameToIndex[name] = idx;
    columns.push_back(ColumnDef{std::move(name), type});
    return true;
}

int Schema::findColumn(std::string_view name) const {
    auto it = nameToIndex.find(std::string(name));
    if (it == nameToIndex.end()) return -1;
    return static_cast<int>(it->second);
}

uint64_t Schema::computeHash() const {
    uint64_t h = mixHash64(columns.size());
    for (const auto& c : columns) {
        h = combineHash64(h, hashBytes(c.name.data(), c.name.size()));
        h = combineHash64(h, static_cast<uint64_t>(c.type));
    }
    return h;
}

ScalarValue TableSnapshot::getCell(size_t colIdx, uint32_t chunkIdx, uint32_t rowIdx) const {
    if (colIdx >= schema.size() || chunkIdx >= chunks.size()) return ScalarValue::makeNull();
    const FrozenChunk& fc = chunks[chunkIdx];
    if (rowIdx >= fc.rowCount) return ScalarValue::makeNull();
    return fc.columns[colIdx]->readCell(rowIdx, dict.get());
}

const HashIndex* TableSnapshot::findIndex(size_t colIdx) const {
    auto it = indexes.find(colIdx);
    return it == indexes.end() ? nullptr : it->second.get();
}

const RangeIndex* TableSnapshot::findRangeIndex(size_t colIdx) const {
    auto it = rangeIndexes.find(colIdx);
    return it == rangeIndexes.end() ? nullptr : it->second.get();
}

const SpatialIndex* TableSnapshot::findSpatialIndex(std::string_view name) const {
    auto it = spatialIndexes.find(std::string(name));
    return it == spatialIndexes.end() ? nullptr : it->second.get();
}

std::vector<uint32_t> TableSnapshot::dirtyChunksSince(uint64_t sinceVersion) const {
    std::vector<uint32_t> out;
    for (uint32_t ci = 0; ci < chunks.size(); ++ci) {
        if (chunks[ci].dirtyVersion > sinceVersion) out.push_back(ci);
    }
    return out;
}

TableStats TableSnapshot::stats() const {
    TableStats st = computeStatsCommon(schema, dict.get(), chunks, totalAliveRows,
                                       indexes.size(), rangeIndexes.size(), spatialIndexes.size());
    for (const auto& kv : indexes) {
        if (kv.second && kv.second->isMapped()) ++st.mappedIndexes;
    }
    for (const auto& kv : rangeIndexes) {
        if (kv.second && kv.second->isMapped()) ++st.mappedIndexes;
    }
    for (const auto& kv : spatialIndexes) {
        if (kv.second && kv.second->isMapped()) ++st.mappedIndexes;
    }
    return st;
}

std::vector<size_t> TableSnapshot::hashIndexCols() const {
    std::vector<size_t> cols;
    for (const auto& kv : indexes) {
        if (kv.second) cols.push_back(kv.first);
    }
    std::sort(cols.begin(), cols.end());
    return cols;
}

std::vector<size_t> TableSnapshot::rangeIndexCols() const {
    std::vector<size_t> cols;
    for (const auto& kv : rangeIndexes) {
        if (kv.second) cols.push_back(kv.first);
    }
    std::sort(cols.begin(), cols.end());
    return cols;
}

std::vector<const SpatialIndex*> TableSnapshot::spatialIndexList() const {
    std::vector<const SpatialIndex*> list;
    for (const auto& kv : spatialIndexes) {
        if (kv.second) list.push_back(kv.second.get());
    }
    std::sort(list.begin(), list.end(), [](const SpatialIndex* a, const SpatialIndex* b) {
        return a->name() < b->name();
    });
    return list;
}

Table::Table(std::vector<ColumnDef> columns)
    : dict_(std::make_shared<StringDictionary>()) {
    for (auto& c : columns) {
        schema_.addColumn(std::move(c.name), c.type);
    }
    schemaHash_ = schema_.computeHash();
}

std::shared_ptr<Table> Table::fromSnapshot(const TableSnapshot& snap) {
    auto tbl = std::make_shared<Table>(snap.schema.columns);
    tbl->dict_ = snap.dict ? snap.dict : std::make_shared<StringDictionary>();
    tbl->totalAliveRows_ = snap.totalAliveRows;
    tbl->version_ = snap.version + 1;
    tbl->structureVersion_ = snap.structureVersion + 1;
    tbl->chunks_.reserve(snap.chunks.size());
    for (const auto& fc : snap.chunks) {
        TableChunk ch;
        ch.chunkId = tbl->nextChunkId_++;
        ch.dirtyVersion = tbl->version_;
        ch.rowCount = fc.rowCount;
        ch.aliveCount = fc.aliveCount;
        ch.aliveWords = fc.aliveWords;
        ch.columns.reserve(fc.columns.size());
        for (const auto& col : fc.columns) {
            ch.columns.push_back(col ? col->clone() : ColumnChunk::create(ColType::I32));
        }
        tbl->chunks_.push_back(std::move(ch));
    }
    for (const auto& kv : snap.indexes) {
        if (kv.second) tbl->indexes_.emplace(kv.first, kv.second->clone());
    }
    for (const auto& kv : snap.rangeIndexes) {
        if (kv.second) tbl->rangeIndexes_.emplace(kv.first, kv.second->clone());
    }
    for (const auto& kv : snap.spatialIndexes) {
        if (kv.second) tbl->spatialIndexes_.emplace(kv.first, kv.second->clone());
    }
    return tbl;
}

TableChunk& Table::ensureTailChunkForAppend() {
    if (chunks_.empty() || chunks_.back().rowCount >= kChunkRows) {
        TableChunk ch;
        ch.chunkId = nextChunkId_++;
        ch.dirtyVersion = version_;
        ch.columns.reserve(schema_.size());
        for (const auto& def : schema_.columns) {
            ch.columns.push_back(ColumnChunk::create(def.type));
        }
        chunks_.push_back(std::move(ch));
        ++structureVersion_;
        return chunks_.back();
    }
    ensureChunkUnique(chunks_.size() - 1);
    return chunks_.back();
}

void Table::ensureChunkUnique(size_t chunkIdx) {
    if (chunkIdx >= chunks_.size()) return;
    TableChunk& ch = chunks_[chunkIdx];
    for (size_t c = 0; c < ch.columns.size(); ++c) {
        (void)ch.mutableColumn(c);
    }
}

std::shared_ptr<HashIndex>& Table::mutableIndex(size_t colIdx) {
    auto& ptr = indexes_[colIdx];
    if (ptr && ptr.use_count() > 1) ptr = ptr->clone();
    return ptr;
}

std::shared_ptr<RangeIndex>& Table::mutableRangeIndex(size_t colIdx) {
    auto& ptr = rangeIndexes_[colIdx];
    if (ptr && ptr.use_count() > 1) ptr = ptr->clone();
    return ptr;
}

std::shared_ptr<SpatialIndex>& Table::mutableSpatialIndex(const std::string& name) {
    auto& ptr = spatialIndexes_[name];
    if (ptr && ptr.use_count() > 1) ptr = ptr->clone();
    return ptr;
}

RowLoc Table::appendRow(std::span<const ScalarValue> values) {
    TableChunk& ch = ensureTailChunkForAppend();
    const uint32_t chunkIdx = static_cast<uint32_t>(chunks_.size() - 1);
    const uint32_t r = ch.rowCount++;
    ch.setAlive(r, true);

    ++version_;
    ch.dirtyVersion = version_;
    const RowLoc loc{chunkIdx, r};
    const size_t nCols = schema_.size();
    for (size_t c = 0; c < nCols; ++c) {
        if (c < values.size() && !values[c].isNull) {
            ch.columns[c]->writeCell(r, values[c], dict_.get());
            if (auto it = indexes_.find(c); it != indexes_.end() && it->second) {
                mutableIndex(c)->insert(HashIndex::cellKey(*ch.columns[c], r), loc);
            }
            if (auto rit = rangeIndexes_.find(c); rit != rangeIndexes_.end() && rit->second) {
                mutableRangeIndex(c)->insert(*ch.columns[c], r, loc, dict_.get());
            }
        } else {
            ch.columns[c]->setNull(r, true);
        }
    }
    for (auto& kv : spatialIndexes_) {
        if (!kv.second) continue;
        double x = 0.0, y = 0.0, z = 0.0;
        if (readSpatialCoords(ch, *kv.second, r, x, y, z)) {
            mutableSpatialIndex(kv.first)->insert(x, y, z, loc);
        }
    }
    ++totalAliveRows_;
    return loc;
}

void Table::appendTypedBatch(uint32_t count, const void* const* colData) {
    if (count == 0) return;
    ++version_;
    uint32_t offset = 0;
    const size_t nCols = schema_.size();
    while (offset < count) {
        TableChunk& ch = ensureTailChunkForAppend();
        const uint32_t chunkIdx = static_cast<uint32_t>(chunks_.size() - 1);
        const uint32_t avail = kChunkRows - ch.rowCount;
        const uint32_t take = std::min(avail, count - offset);
        const uint32_t startRow = ch.rowCount;

        for (size_t c = 0; c < nCols; ++c) {
            const ColType t = schema_.columns[c].type;
            const void* src = colData ? colData[c] : nullptr;
            auto& colPtr = ch.mutableColumn(c);
            if (src && colTypeHasTypedArray(t)) {
                const size_t es = colTypeElemSize(t);
                const uint8_t* srcBytes =
                    static_cast<const uint8_t*>(src) + static_cast<size_t>(offset) * es;
                std::memcpy(colPtr->rawData() + static_cast<size_t>(startRow) * es,
                            srcBytes, static_cast<size_t>(take) * es);
            } else {
                for (uint32_t i = 0; i < take; ++i) {
                    colPtr->setNull(startRow + i, true);
                }
            }
        }
        for (uint32_t i = 0; i < take; ++i) ch.setAlive(startRow + i, true);
        ch.rowCount += take;
        ch.dirtyVersion = version_;
        totalAliveRows_ += take;

        for (size_t c = 0; c < nCols; ++c) {
            if (auto it = indexes_.find(c); it != indexes_.end() && it->second) {
                auto& idx = mutableIndex(c);
                const ColumnChunk& colChunk = *ch.columns[c];
                for (uint32_t i = 0; i < take; ++i) {
                    const uint32_t r = startRow + i;
                    if (!colChunk.isNull(r)) {
                        idx->insert(HashIndex::cellKey(colChunk, r), RowLoc{chunkIdx, r});
                    }
                }
            }
            if (auto rit = rangeIndexes_.find(c); rit != rangeIndexes_.end() && rit->second) {
                auto& ridx = mutableRangeIndex(c);
                const ColumnChunk& colChunk = *ch.columns[c];
                for (uint32_t i = 0; i < take; ++i) {
                    const uint32_t r = startRow + i;
                    if (!colChunk.isNull(r)) {
                        ridx->insert(colChunk, r, RowLoc{chunkIdx, r}, dict_.get());
                    }
                }
            }
        }
        for (auto& kv : spatialIndexes_) {
            if (!kv.second) continue;
            auto& si = mutableSpatialIndex(kv.first);
            for (uint32_t i = 0; i < take; ++i) {
                const uint32_t r = startRow + i;
                double x = 0.0, y = 0.0, z = 0.0;
                if (readSpatialCoords(ch, *si, r, x, y, z)) {
                    si->insert(x, y, z, RowLoc{chunkIdx, r});
                }
            }
        }
        offset += take;
    }
}

bool Table::updateCell(RowLoc loc, size_t colIdx, const ScalarValue& v) {
    if (loc.chunkIdx >= chunks_.size() || colIdx >= schema_.size()) return false;
    TableChunk& ch = chunks_[loc.chunkIdx];
    if (!ch.isAlive(loc.rowIdx)) return false;

    std::vector<std::pair<std::string, std::array<double, 3>>> oldSpatial;
    for (const auto& kv : spatialIndexes_) {
        if (kv.second && spatialUsesColumn(*kv.second, colIdx)) {
            double ox = 0.0, oy = 0.0, oz = 0.0;
            if (readSpatialCoords(ch, *kv.second, loc.rowIdx, ox, oy, oz)) {
                oldSpatial.emplace_back(kv.first, std::array<double, 3>{ox, oy, oz});
            }
        }
    }

    auto& colPtr = ch.mutableColumn(colIdx);
    const bool wasNull = colPtr->isNull(loc.rowIdx);
    if (!wasNull) {
        if (auto idxIt = indexes_.find(colIdx); idxIt != indexes_.end() && idxIt->second) {
            mutableIndex(colIdx)->erase(HashIndex::cellKey(*colPtr, loc.rowIdx), loc);
        }
        if (auto rIt = rangeIndexes_.find(colIdx); rIt != rangeIndexes_.end() && rIt->second) {
            mutableRangeIndex(colIdx)->erase(*colPtr, loc.rowIdx, loc, dict_.get());
        }
    }

    colPtr->writeCell(loc.rowIdx, v, dict_.get());

    const bool isNowNull = colPtr->isNull(loc.rowIdx);
    if (!isNowNull) {
        if (auto idxIt = indexes_.find(colIdx); idxIt != indexes_.end() && idxIt->second) {
            mutableIndex(colIdx)->insert(HashIndex::cellKey(*colPtr, loc.rowIdx), loc);
        }
        if (auto rIt = rangeIndexes_.find(colIdx); rIt != rangeIndexes_.end() && rIt->second) {
            mutableRangeIndex(colIdx)->insert(*colPtr, loc.rowIdx, loc, dict_.get());
        }
    }
    for (const auto& item : oldSpatial) {
        mutableSpatialIndex(item.first)->erase(item.second[0], item.second[1], item.second[2], loc);
    }
    for (const auto& kv : spatialIndexes_) {
        if (kv.second && spatialUsesColumn(*kv.second, colIdx)) {
            double nx = 0.0, ny = 0.0, nz = 0.0;
            if (readSpatialCoords(ch, *kv.second, loc.rowIdx, nx, ny, nz)) {
                mutableSpatialIndex(kv.first)->insert(nx, ny, nz, loc);
            }
        }
    }

    ++version_;
    ch.dirtyVersion = version_;
    return true;
}

bool Table::removeRow(RowLoc loc) {
    if (loc.chunkIdx >= chunks_.size()) return false;
    TableChunk& ch = chunks_[loc.chunkIdx];
    if (!ch.isAlive(loc.rowIdx)) return false;

    for (size_t c = 0; c < schema_.size(); ++c) {
        if (ch.columns[c]->isNull(loc.rowIdx)) continue;
        if (auto idxIt = indexes_.find(c); idxIt != indexes_.end() && idxIt->second) {
            mutableIndex(c)->erase(HashIndex::cellKey(*ch.columns[c], loc.rowIdx), loc);
        }
        if (auto rIt = rangeIndexes_.find(c); rIt != rangeIndexes_.end() && rIt->second) {
            mutableRangeIndex(c)->erase(*ch.columns[c], loc.rowIdx, loc, dict_.get());
        }
    }
    for (const auto& kv : spatialIndexes_) {
        if (!kv.second) continue;
        double x = 0.0, y = 0.0, z = 0.0;
        if (readSpatialCoords(ch, *kv.second, loc.rowIdx, x, y, z)) {
            mutableSpatialIndex(kv.first)->erase(x, y, z, loc);
        }
    }
    ch.setAlive(loc.rowIdx, false);
    --totalAliveRows_;
    ++version_;
    ch.dirtyVersion = version_;
    return true;
}

bool Table::resolveAliveRow(size_t aliveOrdinal, RowLoc& out) const {
    if (aliveOrdinal >= totalAliveRows_) return false;
    size_t acc = 0;
    for (uint32_t ci = 0; ci < chunks_.size(); ++ci) {
        const TableChunk& ch = chunks_[ci];
        if (acc + ch.aliveCount <= aliveOrdinal) {
            acc += ch.aliveCount;
            continue;
        }
        if (ch.allAlive()) {
            out = RowLoc{ci, static_cast<uint32_t>(aliveOrdinal - acc)};
            return true;
        }
        for (uint32_t r = 0; r < ch.rowCount; ++r) {
            if (ch.isAlive(r)) {
                if (acc == aliveOrdinal) {
                    out = RowLoc{ci, r};
                    return true;
                }
                ++acc;
            }
        }
    }
    return false;
}

ScalarValue Table::getCell(RowLoc loc, size_t colIdx) const {
    if (loc.chunkIdx >= chunks_.size() || colIdx >= schema_.size()) return ScalarValue::makeNull();
    const TableChunk& ch = chunks_[loc.chunkIdx];
    if (!ch.isAlive(loc.rowIdx)) return ScalarValue::makeNull();
    return ch.columns[colIdx]->readCell(loc.rowIdx, dict_.get());
}

void Table::compact() {
    bool needsCompact = false;
    for (size_t ci = 0; ci < chunks_.size(); ++ci) {
        const TableChunk& ch = chunks_[ci];
        if (!ch.allAlive() || (ci + 1 < chunks_.size() && ch.rowCount < kChunkRows)) {
            needsCompact = true;
            break;
        }
    }
    if (!needsCompact) return;

    ++version_;
    ++structureVersion_;
    std::vector<TableChunk> nextChunks;
    const size_t nCols = schema_.size();

    auto appendNewChunk = [&]() -> TableChunk& {
        TableChunk ch;
        ch.chunkId = nextChunkId_++;
        ch.dirtyVersion = version_;
        ch.columns.reserve(nCols);
        for (const auto& def : schema_.columns) {
            ch.columns.push_back(ColumnChunk::create(def.type));
        }
        nextChunks.push_back(std::move(ch));
        return nextChunks.back();
    };

    for (const TableChunk& src : chunks_) {
        if (src.aliveCount == 0) continue;
        for (uint32_t r = 0; r < src.rowCount; ++r) {
            if (!src.isAlive(r)) continue;
            if (nextChunks.empty() || nextChunks.back().rowCount >= kChunkRows) {
                appendNewChunk();
            }
            TableChunk& dst = nextChunks.back();
            const uint32_t dr = dst.rowCount++;
            dst.setAlive(dr, true);
            for (size_t c = 0; c < nCols; ++c) {
                dst.columns[c]->copyRowFrom(dr, *src.columns[c], r);
            }
        }
    }

    chunks_ = std::move(nextChunks);
    std::vector<size_t> indexCols, rangeCols;
    std::vector<std::string> spatialNames;
    for (const auto& kv : indexes_) if (kv.second) indexCols.push_back(kv.first);
    for (const auto& kv : rangeIndexes_) if (kv.second) rangeCols.push_back(kv.first);
    for (const auto& kv : spatialIndexes_) if (kv.second) spatialNames.push_back(kv.first);
    for (size_t c : indexCols) rebuildIndex(c);
    for (size_t c : rangeCols) rebuildRangeIndex(c);
    for (const auto& nm : spatialNames) rebuildSpatialIndex(nm);
}

void Table::clear() {
    chunks_.clear();
    totalAliveRows_ = 0;
    ++version_;
    ++structureVersion_;
    for (auto& kv : indexes_) {
        if (kv.second) mutableIndex(kv.first)->clear(64);
    }
    for (auto& kv : rangeIndexes_) {
        if (kv.second) mutableRangeIndex(kv.first)->clear();
    }
    for (auto& kv : spatialIndexes_) {
        if (kv.second) mutableSpatialIndex(kv.first)->clear(64);
    }
}

TableStats Table::stats() const {
    return computeStatsCommon(schema_, dict_.get(), chunks_, totalAliveRows_,
                              indexes_.size(), rangeIndexes_.size(), spatialIndexes_.size());
}

void Table::rebuildIndex(size_t colIdx) {
    auto idx = std::make_shared<HashIndex>(colIdx, schema_.columns[colIdx].type);
    idx->clear(totalAliveRows_);
    for (uint32_t ci = 0; ci < chunks_.size(); ++ci) {
        const TableChunk& ch = chunks_[ci];
        const ColumnChunk& colChunk = *ch.columns[colIdx];
        for (uint32_t r = 0; r < ch.rowCount; ++r) {
            if (ch.isAlive(r) && !colChunk.isNull(r)) {
                idx->insert(HashIndex::cellKey(colChunk, r), RowLoc{ci, r});
            }
        }
    }
    indexes_[colIdx] = std::move(idx);
}

void Table::rebuildRangeIndex(size_t colIdx) {
    auto idx = std::make_shared<RangeIndex>(colIdx, schema_.columns[colIdx].type);
    for (uint32_t ci = 0; ci < chunks_.size(); ++ci) {
        const TableChunk& ch = chunks_[ci];
        const ColumnChunk& colChunk = *ch.columns[colIdx];
        for (uint32_t r = 0; r < ch.rowCount; ++r) {
            if (ch.isAlive(r) && !colChunk.isNull(r)) {
                idx->insert(colChunk, r, RowLoc{ci, r}, dict_.get());
            }
        }
    }
    idx->sortIfNeeded();
    rangeIndexes_[colIdx] = std::move(idx);
}

void Table::rebuildSpatialIndex(const std::string& name) {
    auto it = spatialIndexes_.find(name);
    if (it == spatialIndexes_.end() || !it->second) return;
    std::vector<size_t> cols(it->second->cols().begin(),
                             it->second->cols().begin() + it->second->dims());
    auto si = std::make_shared<SpatialIndex>(name, std::move(cols), it->second->cellSize());
    si->clear(totalAliveRows_);
    for (uint32_t ci = 0; ci < chunks_.size(); ++ci) {
        const TableChunk& ch = chunks_[ci];
        for (uint32_t r = 0; r < ch.rowCount; ++r) {
            if (!ch.isAlive(r)) continue;
            double x = 0.0, y = 0.0, z = 0.0;
            if (readSpatialCoords(ch, *si, r, x, y, z)) {
                si->insert(x, y, z, RowLoc{ci, r});
            }
        }
    }
    spatialIndexes_[name] = std::move(si);
}

bool Table::createIndex(size_t colIdx) {
    if (colIdx >= schema_.size()) return false;
    if (hasIndex(colIdx)) return true;
    rebuildIndex(colIdx);
    return true;
}

bool Table::createIndex(std::string_view name) {
    int idx = findColumn(name);
    return idx >= 0 && createIndex(static_cast<size_t>(idx));
}

bool Table::hasIndex(size_t colIdx) const {
    auto it = indexes_.find(colIdx);
    return it != indexes_.end() && it->second != nullptr;
}

bool Table::dropIndex(size_t colIdx) { return indexes_.erase(colIdx) > 0; }

bool Table::createRangeIndex(size_t colIdx) {
    if (colIdx >= schema_.size()) return false;
    if (hasRangeIndex(colIdx)) return true;
    rebuildRangeIndex(colIdx);
    return true;
}

bool Table::createRangeIndex(std::string_view name) {
    int idx = findColumn(name);
    return idx >= 0 && createRangeIndex(static_cast<size_t>(idx));
}

bool Table::hasRangeIndex(size_t colIdx) const {
    auto it = rangeIndexes_.find(colIdx);
    return it != rangeIndexes_.end() && it->second != nullptr;
}

bool Table::dropRangeIndex(size_t colIdx) { return rangeIndexes_.erase(colIdx) > 0; }

bool Table::createSpatialIndex(std::string name, std::vector<size_t> cols, double cellSize) {
    if (name.empty() || cols.size() < 2 || cols.size() > 3) return false;
    for (size_t c : cols) {
        if (c >= schema_.size()) return false;
        const ColType ct = schema_.columns[c].type;
        if (ct == ColType::String || ct == ColType::Dict || ct == ColType::Bool) return false;
    }
    auto si = std::make_shared<SpatialIndex>(name, std::move(cols), cellSize);
    spatialIndexes_[name] = std::move(si);
    rebuildSpatialIndex(name);
    return true;
}

bool Table::hasSpatialIndex(std::string_view name) const {
    return findSpatialIndex(name) != nullptr;
}

bool Table::dropSpatialIndex(std::string_view name) {
    return spatialIndexes_.erase(std::string(name)) > 0;
}

const SpatialIndex* Table::findSpatialIndex(std::string_view name) const {
    auto it = spatialIndexes_.find(std::string(name));
    return it == spatialIndexes_.end() ? nullptr : it->second.get();
}

std::vector<uint32_t> Table::dirtyChunksSince(uint64_t sinceVersion) const {
    std::vector<uint32_t> out;
    for (uint32_t ci = 0; ci < chunks_.size(); ++ci) {
        if (chunks_[ci].dirtyVersion > sinceVersion) out.push_back(ci);
    }
    return out;
}

uint64_t Table::chunkDirtyVersion(size_t chunkIdx) const {
    return chunkIdx < chunks_.size() ? chunks_[chunkIdx].dirtyVersion : 0;
}

void Table::markChunkDirty(size_t chunkIdx) {
    if (chunkIdx >= chunks_.size()) return;
    ++version_;
    chunks_[chunkIdx].dirtyVersion = version_;
}

void Table::notifyColumnUpdated(size_t colIdx) {
    ++version_;
    if (hasIndex(colIdx)) rebuildIndex(colIdx);
    if (hasRangeIndex(colIdx)) rebuildRangeIndex(colIdx);
    std::vector<std::string> affectedSpatial;
    for (const auto& kv : spatialIndexes_) {
        if (kv.second && spatialUsesColumn(*kv.second, colIdx)) {
            affectedSpatial.push_back(kv.first);
        }
    }
    for (const auto& nm : affectedSpatial) rebuildSpatialIndex(nm);
}

std::shared_ptr<const TableSnapshot> Table::snapshot() const {
    auto snap = std::make_shared<TableSnapshot>();
    snap->schema = schema_;
    snap->dict = dict_;
    snap->totalAliveRows = totalAliveRows_;
    snap->schemaHash = schemaHash_;
    snap->version = version_;
    snap->structureVersion = structureVersion_;
    snap->chunks.reserve(chunks_.size());
    for (const TableChunk& ch : chunks_) {
        TableSnapshot::FrozenChunk fc;
        fc.chunkId = ch.chunkId;
        fc.dirtyVersion = ch.dirtyVersion;
        fc.rowCount = ch.rowCount;
        fc.aliveCount = ch.aliveCount;
        fc.aliveWords = ch.aliveWords;
        fc.columns.reserve(ch.columns.size());
        for (const auto& col : ch.columns) fc.columns.push_back(col);
        snap->chunks.push_back(std::move(fc));
    }
    for (const auto& kv : indexes_) {
        if (kv.second) snap->indexes.emplace(kv.first, kv.second);
    }
    for (const auto& kv : rangeIndexes_) {
        if (kv.second) {
            kv.second->sortIfNeeded();
            snap->rangeIndexes.emplace(kv.first, kv.second);
        }
    }
    for (const auto& kv : spatialIndexes_) {
        if (kv.second) snap->spatialIndexes.emplace(kv.first, kv.second);
    }
    return snap;
}

}  // namespace bro::table
