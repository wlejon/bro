#include "table/table.h"

#include <cstring>

namespace bro::table {

void Schema::addColumn(std::string name, ColType type) {
    const size_t idx = columns.size();
    nameToIndex[name] = idx;
    columns.push_back(ColumnDef{std::move(name), type});
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

Table::Table(std::vector<ColumnDef> columns)
    : dict_(std::make_shared<StringDictionary>()) {
    for (auto& c : columns) {
        schema_.addColumn(std::move(c.name), c.type);
    }
    schemaHash_ = schema_.computeHash();
}

TableChunk& Table::ensureTailChunkForAppend() {
    if (chunks_.empty() || chunks_.back().rowCount >= kChunkRows) {
        TableChunk ch;
        ch.columns.reserve(schema_.size());
        for (const auto& def : schema_.columns) {
            ch.columns.push_back(ColumnChunk::create(def.type));
        }
        chunks_.push_back(std::move(ch));
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
    if (ptr && ptr.use_count() > 1) {
        ptr = ptr->clone();
    }
    return ptr;
}

RowLoc Table::appendRow(std::span<const ScalarValue> values) {
    TableChunk& ch = ensureTailChunkForAppend();
    const uint32_t chunkIdx = static_cast<uint32_t>(chunks_.size() - 1);
    const uint32_t r = ch.rowCount++;
    ch.setAlive(r, true);

    const size_t nCols = schema_.size();
    for (size_t c = 0; c < nCols; ++c) {
        if (c < values.size() && !values[c].isNull) {
            ch.columns[c]->writeCell(r, values[c], dict_.get());
        }
        auto it = indexes_.find(c);
        if (it != indexes_.end() && it->second) {
            auto& idx = mutableIndex(c);
            idx->insert(HashIndex::cellKey(*ch.columns[c], r), RowLoc{chunkIdx, r});
        }
    }
    ++totalAliveRows_;
    ++version_;
    return RowLoc{chunkIdx, r};
}

void Table::appendTypedBatch(uint32_t count, const void* const* colData) {
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
            if (src && colTypeHasTypedArray(t)) {
                const size_t es = colTypeElemSize(t);
                const uint8_t* srcBytes = static_cast<const uint8_t*>(src) + static_cast<size_t>(offset) * es;
                std::memcpy(ch.columns[c]->rawData() + static_cast<size_t>(startRow) * es,
                            srcBytes, static_cast<size_t>(take) * es);
            }
        }
        for (uint32_t i = 0; i < take; ++i) {
            ch.setAlive(startRow + i, true);
        }
        ch.rowCount += take;
        totalAliveRows_ += take;

        for (size_t c = 0; c < nCols; ++c) {
            auto it = indexes_.find(c);
            if (it != indexes_.end() && it->second) {
                auto& idx = mutableIndex(c);
                const ColumnChunk& colChunk = *ch.columns[c];
                for (uint32_t i = 0; i < take; ++i) {
                    const uint32_t r = startRow + i;
                    idx->insert(HashIndex::cellKey(colChunk, r), RowLoc{chunkIdx, r});
                }
            }
        }
        offset += take;
    }
    if (count > 0) ++version_;
}

bool Table::updateCell(RowLoc loc, size_t colIdx, const ScalarValue& v) {
    if (loc.chunkIdx >= chunks_.size() || colIdx >= schema_.size()) return false;
    TableChunk& ch = chunks_[loc.chunkIdx];
    if (!ch.isAlive(loc.rowIdx)) return false;

    auto& colPtr = ch.mutableColumn(colIdx);
    auto idxIt = indexes_.find(colIdx);
    if (idxIt != indexes_.end() && idxIt->second) {
        auto& idx = mutableIndex(colIdx);
        idx->erase(HashIndex::cellKey(*colPtr, loc.rowIdx), loc);
        colPtr->writeCell(loc.rowIdx, v, dict_.get());
        idx->insert(HashIndex::cellKey(*colPtr, loc.rowIdx), loc);
    } else {
        colPtr->writeCell(loc.rowIdx, v, dict_.get());
    }
    ++version_;
    return true;
}

bool Table::removeRow(RowLoc loc) {
    if (loc.chunkIdx >= chunks_.size()) return false;
    TableChunk& ch = chunks_[loc.chunkIdx];
    if (!ch.isAlive(loc.rowIdx)) return false;

    for (size_t c = 0; c < schema_.size(); ++c) {
        auto idxIt = indexes_.find(c);
        if (idxIt != indexes_.end() && idxIt->second) {
            auto& idx = mutableIndex(c);
            idx->erase(HashIndex::cellKey(*ch.columns[c], loc.rowIdx), loc);
        }
    }
    ch.setAlive(loc.rowIdx, false);
    --totalAliveRows_;
    ++version_;
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

    std::vector<TableChunk> nextChunks;
    const size_t nCols = schema_.size();

    auto appendNewChunk = [&]() -> TableChunk& {
        TableChunk ch;
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
    std::vector<size_t> indexCols;
    for (const auto& kv : indexes_) {
        if (kv.second) indexCols.push_back(kv.first);
    }
    for (size_t c : indexCols) {
        rebuildIndex(c);
    }
    ++version_;
}

void Table::rebuildIndex(size_t colIdx) {
    auto idx = std::make_shared<HashIndex>(colIdx, schema_.columns[colIdx].type);
    idx->clear(totalAliveRows_);
    for (uint32_t ci = 0; ci < chunks_.size(); ++ci) {
        const TableChunk& ch = chunks_[ci];
        const ColumnChunk& colChunk = *ch.columns[colIdx];
        for (uint32_t r = 0; r < ch.rowCount; ++r) {
            if (!ch.isAlive(r)) continue;
            idx->insert(HashIndex::cellKey(colChunk, r), RowLoc{ci, r});
        }
    }
    indexes_[colIdx] = std::move(idx);
}

bool Table::createIndex(size_t colIdx) {
    if (colIdx >= schema_.size()) return false;
    if (indexes_.find(colIdx) != indexes_.end() && indexes_[colIdx]) return true;
    rebuildIndex(colIdx);
    return true;
}

bool Table::createIndex(std::string_view name) {
    int idx = findColumn(name);
    if (idx < 0) return false;
    return createIndex(static_cast<size_t>(idx));
}

bool Table::hasIndex(size_t colIdx) const {
    auto it = indexes_.find(colIdx);
    return it != indexes_.end() && it->second != nullptr;
}

bool Table::dropIndex(size_t colIdx) {
    return indexes_.erase(colIdx) > 0;
}

void Table::notifyColumnUpdated(size_t colIdx) {
    ++version_;
    auto it = indexes_.find(colIdx);
    if (it != indexes_.end() && it->second) {
        rebuildIndex(colIdx);
    }
}

std::shared_ptr<const TableSnapshot> Table::snapshot() const {
    auto snap = std::make_shared<TableSnapshot>();
    snap->schema = schema_;
    snap->dict = dict_;
    snap->totalAliveRows = totalAliveRows_;
    snap->schemaHash = schemaHash_;
    snap->version = version_;
    snap->chunks.reserve(chunks_.size());
    for (const TableChunk& ch : chunks_) {
        TableSnapshot::FrozenChunk fc;
        fc.rowCount = ch.rowCount;
        fc.aliveCount = ch.aliveCount;
        fc.aliveWords = ch.aliveWords;
        fc.columns.reserve(ch.columns.size());
        for (const auto& col : ch.columns) {
            fc.columns.push_back(col);
        }
        snap->chunks.push_back(std::move(fc));
    }
    for (const auto& kv : indexes_) {
        if (kv.second) {
            snap->indexes.emplace(kv.first, kv.second);
        }
    }
    return snap;
}

}  // namespace bro::table
