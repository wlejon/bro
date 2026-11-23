#include "table/table_range_index.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <limits>
#include <queue>

namespace bro::table {

namespace {

bool locLess(RowLoc a, RowLoc b) {
    return a.chunkIdx != b.chunkIdx ? (a.chunkIdx < b.chunkIdx) : (a.rowIdx < b.rowIdx);
}

bool locEq(RowLoc a, RowLoc b) {
    return a.chunkIdx == b.chunkIdx && a.rowIdx == b.rowIdx;
}

bool extractCoords(double& x, double& y, double& z, uint8_t dims) {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (dims >= 3) {
        if (!std::isfinite(z)) return false;
    } else {
        z = 0.0;
    }
    return true;
}

}  // namespace

RangeIndex::RangeIndex(size_t colIdx, ColType colType)
    : colIdx_(colIdx), colType_(colType) {}

std::shared_ptr<RangeIndex> RangeIndex::clone() const {
    sortIfNeeded();
    auto copy = std::make_shared<RangeIndex>();
    copy->colIdx_ = colIdx_;
    copy->colType_ = colType_;
    copy->numEntries_ = numEntries_;
    copy->strEntries_ = strEntries_;
    copy->sorted_ = true;
    return copy;
}

void RangeIndex::clear() {
    numEntries_.clear();
    strEntries_.clear();
    sorted_ = true;
}

bool RangeIndex::makeNumEntry(ColType colType, const ScalarValue& v, RowLoc loc,
                              NumEntry& out) {
    if (v.isNull) return false;
    out.loc = loc;
    switch (colType) {
    case ColType::I32:
        out.iKey = (v.type == ColType::F32 || v.type == ColType::F64)
                       ? static_cast<int32_t>(v.f64)
                       : static_cast<int32_t>(v.i64);
        return true;
    case ColType::U32:
        out.uKey = (v.type == ColType::F32 || v.type == ColType::F64)
                       ? static_cast<uint32_t>(static_cast<int64_t>(v.f64))
                       : static_cast<uint32_t>(v.u64);
        return true;
    case ColType::I64:
        out.iKey = (v.type == ColType::F32 || v.type == ColType::F64)
                       ? static_cast<int64_t>(v.f64)
                       : v.i64;
        return true;
    case ColType::Bool:
        out.uKey = (v.i64 != 0 || v.u64 != 0 || v.f64 != 0.0) ? 1ULL : 0ULL;
        return true;
    case ColType::F32: {
        double fv = static_cast<double>(static_cast<float>(v.f64));
        if (std::isnan(fv)) return false;
        out.fKey = (fv == 0.0) ? 0.0 : fv;
        return true;
    }
    case ColType::F64: {
        double dv = colTypeIsFloat(v.type)
                        ? v.f64
                        : (colTypeIsUnsignedInt(v.type) ? static_cast<double>(v.u64)
                                                        : static_cast<double>(v.i64));
        if (std::isnan(dv)) return false;
        out.fKey = (dv == 0.0) ? 0.0 : dv;
        return true;
    }
    case ColType::Dict:
    case ColType::String:
        return false;
    }
    return false;
}

int RangeIndex::compareNum(const NumEntry& a, const NumEntry& b) const {
    if (colTypeIsFloat(colType_)) {
        if (a.fKey < b.fKey) return -1;
        if (a.fKey > b.fKey) return 1;
        return 0;
    }
    if (colTypeIsUnsignedInt(colType_)) {
        if (a.uKey < b.uKey) return -1;
        if (a.uKey > b.uKey) return 1;
        return 0;
    }
    if (a.iKey < b.iKey) return -1;
    if (a.iKey > b.iKey) return 1;
    return 0;
}

void RangeIndex::convertBound(ColType colType, const ScalarValue& v, bool isLo, bool& inc,
                              NumEntry& out, bool& alwaysEmpty, bool& unbounded) {
    alwaysEmpty = false;
    unbounded = false;
    out = NumEntry{};
    if (v.isNull) {
        alwaysEmpty = true;
        return;
    }

    if (colTypeIsFloat(colType)) {
        const double dv = colTypeIsFloat(v.type)
                              ? v.f64
                              : (colTypeIsUnsignedInt(v.type) ? static_cast<double>(v.u64)
                                                              : static_cast<double>(v.i64));
        if (std::isnan(dv)) {
            alwaysEmpty = true;
            return;
        }
        if (colType == ColType::F32) {
            constexpr double kF32Max = static_cast<double>(FLT_MAX);
            if (dv > kF32Max) {
                if (isLo) alwaysEmpty = true;
                else unbounded = true;
                return;
            }
            if (dv < -kF32Max) {
                if (isLo) unbounded = true;
                else alwaysEmpty = true;
                return;
            }
            const float fv = static_cast<float>(dv);
            if (static_cast<double>(fv) != dv) inc = true;
            out.fKey = (fv == 0.0f) ? 0.0 : static_cast<double>(fv);
            return;
        }
        out.fKey = (dv == 0.0) ? 0.0 : dv;
        return;
    }

    // Integer / Bool target column
    if (colTypeIsFloat(v.type)) {
        const double d = v.f64;
        if (std::isnan(d)) {
            alwaysEmpty = true;
            return;
        }
        double loLim = 0.0;
        double hiLim = 0.0;
        bool hiExclusiveLim = false;
        if (colType == ColType::I32) {
            loLim = -2147483648.0;
            hiLim = 2147483647.0;
        } else if (colType == ColType::U32) {
            loLim = 0.0;
            hiLim = 4294967295.0;
        } else if (colType == ColType::Bool) {
            loLim = 0.0;
            hiLim = 1.0;
        } else {  // ColType::I64
            loLim = -9223372036854775808.0;
            hiLim = 9223372036854775808.0;  // 2^63 (exact IEEE double, INT64_MAX + 1)
            hiExclusiveLim = true;
        }
        if (d < loLim) {
            if (isLo) unbounded = true;
            else alwaysEmpty = true;
            return;
        }
        if (hiExclusiveLim ? (d >= hiLim) : (d > hiLim)) {
            if (isLo) alwaysEmpty = true;
            else unbounded = true;
            return;
        }
        double snapped = d;
        if (std::trunc(d) != d) {
            snapped = isLo ? std::ceil(d) : std::floor(d);
            inc = true;
        }
        if (colType == ColType::U32 || colType == ColType::Bool) {
            out.uKey = static_cast<uint64_t>(snapped);
        } else {
            out.iKey = static_cast<int64_t>(snapped);
        }
        return;
    }

    if (colType == ColType::I64) {
        out.iKey = colTypeIsUnsignedInt(v.type) ? static_cast<int64_t>(v.u64) : v.i64;
        return;
    }

    if (colType == ColType::U32 || colType == ColType::Bool) {
        if (!colTypeIsUnsignedInt(v.type) && v.i64 < 0) {
            if (isLo) unbounded = true;
            else alwaysEmpty = true;
            return;
        }
        const uint64_t uv = colTypeIsUnsignedInt(v.type) ? v.u64 : static_cast<uint64_t>(v.i64);
        const uint64_t maxU = (colType == ColType::Bool) ? 1ULL : static_cast<uint64_t>(UINT32_MAX);
        if (uv > maxU) {
            if (isLo) alwaysEmpty = true;
            else unbounded = true;
            return;
        }
        out.uKey = uv;
        return;
    }

    // ColType::I32
    if (colTypeIsUnsignedInt(v.type) && v.u64 > static_cast<uint64_t>(INT32_MAX)) {
        if (isLo) alwaysEmpty = true;
        else unbounded = true;
        return;
    }
    const int64_t iv = colTypeIsUnsignedInt(v.type) ? static_cast<int64_t>(v.u64) : v.i64;
    if (iv < INT32_MIN) {
        if (isLo) unbounded = true;
        else alwaysEmpty = true;
        return;
    }
    if (iv > INT32_MAX) {
        if (isLo) alwaysEmpty = true;
        else unbounded = true;
        return;
    }
    out.iKey = iv;
}

void RangeIndex::insert(const ColumnChunk& chunk, uint32_t row, RowLoc loc,
                        const StringDictionary* dict) {
    if (chunk.isNull(row)) return;
    ScalarValue sv = chunk.readCell(row, dict);
    if (sv.isNull) return;
    if (isStringOrdered()) {
        StrEntry e{std::move(sv.str), loc};
        if (sorted_ && !strEntries_.empty()) {
            const auto& bk = strEntries_.back();
            if (e.key < bk.key || (e.key == bk.key && locLess(e.loc, bk.loc))) {
                sorted_ = false;
            }
        }
        strEntries_.push_back(std::move(e));
        return;
    }
    NumEntry e;
    if (!makeNumEntry(colType_, sv, loc, e)) return;
    if (sorted_ && !numEntries_.empty()) {
        const NumEntry& bk = numEntries_.back();
        const int cmp = compareNum(e, bk);
        if (cmp < 0 || (cmp == 0 && locLess(e.loc, bk.loc))) {
            sorted_ = false;
        }
    }
    numEntries_.push_back(e);
}

bool RangeIndex::erase(const ColumnChunk& chunk, uint32_t row, RowLoc loc,
                       const StringDictionary* dict) {
    if (chunk.isNull(row)) return false;
    ScalarValue sv = chunk.readCell(row, dict);
    if (sv.isNull) return false;
    if (isStringOrdered()) {
        sortIfNeeded();
        auto loIt = std::lower_bound(
            strEntries_.begin(), strEntries_.end(), sv.str,
            [](const StrEntry& a, const std::string& key) { return a.key < key; });
        for (auto it = loIt; it != strEntries_.end() && it->key == sv.str; ++it) {
            if (locEq(it->loc, loc)) {
                strEntries_.erase(it);
                return true;
            }
        }
        return false;
    }
    NumEntry key;
    if (!makeNumEntry(colType_, sv, loc, key)) return false;
    sortIfNeeded();
    auto loIt = std::lower_bound(
        numEntries_.begin(), numEntries_.end(), key,
        [this](const NumEntry& a, const NumEntry& b) { return compareNum(a, b) < 0; });
    for (auto it = loIt; it != numEntries_.end() && compareNum(*it, key) == 0; ++it) {
        if (locEq(it->loc, loc)) {
            numEntries_.erase(it);
            return true;
        }
    }
    return false;
}

void RangeIndex::sortIfNeeded() const {
    if (sorted_) return;
    if (isStringOrdered()) {
        std::sort(strEntries_.begin(), strEntries_.end(),
                  [](const StrEntry& a, const StrEntry& b) {
                      if (a.key != b.key) return a.key < b.key;
                      return locLess(a.loc, b.loc);
                  });
    } else {
        std::sort(numEntries_.begin(), numEntries_.end(),
                  [this](const NumEntry& a, const NumEntry& b) {
                      const int c = compareNum(a, b);
                      if (c != 0) return c < 0;
                      return locLess(a.loc, b.loc);
                  });
    }
    sorted_ = true;
}

void RangeIndex::lookupRange(const ScalarValue* lo, bool incLo,
                             const ScalarValue* hi, bool incHi,
                             const StringDictionary* dict,
                             std::vector<RowLoc>& out) const {
    sortIfNeeded();
    if (isStringOrdered()) {
        auto getText = [&](const ScalarValue& s) -> std::string {
            if (s.type == ColType::Dict && s.str.empty() && dict) {
                return dict->get(static_cast<uint32_t>(s.u64));
            }
            return s.str;
        };
        auto first = strEntries_.begin();
        auto last = strEntries_.end();
        if (lo) {
            std::string loStr = getText(*lo);
            first = incLo
                        ? std::lower_bound(first, last, loStr,
                                           [](const StrEntry& a, const std::string& k) { return a.key < k; })
                        : std::upper_bound(first, last, loStr,
                                           [](const std::string& k, const StrEntry& a) { return k < a.key; });
        }
        if (hi && first < last) {
            std::string hiStr = getText(*hi);
            last = incHi
                       ? std::upper_bound(first, last, hiStr,
                                          [](const std::string& k, const StrEntry& a) { return k < a.key; })
                       : std::lower_bound(first, last, hiStr,
                                          [](const StrEntry& a, const std::string& k) { return a.key < k; });
        }
        for (auto it = first; it < last; ++it) {
            out.push_back(it->loc);
        }
        return;
    }

    auto first = numEntries_.begin();
    auto last = numEntries_.end();
    if (lo) {
        NumEntry loEntry;
        bool alwaysEmpty = false;
        bool unbounded = false;
        bool incLoAdj = incLo;
        convertBound(colType_, *lo, true, incLoAdj, loEntry, alwaysEmpty, unbounded);
        if (alwaysEmpty) return;
        if (!unbounded) {
            first = incLoAdj
                        ? std::lower_bound(first, last, loEntry,
                                           [this](const NumEntry& a, const NumEntry& b) {
                                               return compareNum(a, b) < 0;
                                           })
                        : std::upper_bound(first, last, loEntry,
                                           [this](const NumEntry& a, const NumEntry& b) {
                                               return compareNum(a, b) < 0;
                                           });
        }
    }
    if (hi && first < last) {
        NumEntry hiEntry;
        bool alwaysEmpty = false;
        bool unbounded = false;
        bool incHiAdj = incHi;
        convertBound(colType_, *hi, false, incHiAdj, hiEntry, alwaysEmpty, unbounded);
        if (alwaysEmpty) return;
        if (!unbounded) {
            last = incHiAdj
                       ? std::upper_bound(first, last, hiEntry,
                                          [this](const NumEntry& a, const NumEntry& b) {
                                              return compareNum(a, b) < 0;
                                          })
                       : std::lower_bound(first, last, hiEntry,
                                          [this](const NumEntry& a, const NumEntry& b) {
                                              return compareNum(a, b) < 0;
                                          });
        }
    }
    for (auto it = first; it < last; ++it) {
        out.push_back(it->loc);
    }
}

void RangeIndex::forEachOrdered(bool descending,
                                const std::function<bool(RowLoc)>& visit) const {
    sortIfNeeded();
    if (isStringOrdered()) {
        if (!descending) {
            for (const auto& e : strEntries_) {
                if (!visit(e.loc)) return;
            }
        } else {
            for (auto it = strEntries_.rbegin(); it != strEntries_.rend(); ++it) {
                if (!visit(it->loc)) return;
            }
        }
        return;
    }
    if (!descending) {
        for (const auto& e : numEntries_) {
            if (!visit(e.loc)) return;
        }
    } else {
        for (auto it = numEntries_.rbegin(); it != numEntries_.rend(); ++it) {
            if (!visit(it->loc)) return;
        }
    }
}

SpatialIndex::SpatialIndex(std::string name, std::vector<size_t> cols, double cellSize)
    : name_(std::move(name)),
      cellSize_(cellSize > 1e-9 ? cellSize : 16.0) {
    dims_ = (cols.size() >= 3) ? 3 : 2;
    for (size_t i = 0; i < std::min<size_t>(cols.size(), 3); ++i) {
        cols_[i] = cols[i];
    }
    clear(64);
}

std::shared_ptr<SpatialIndex> SpatialIndex::clone() const {
    auto copy = std::make_shared<SpatialIndex>();
    copy->name_ = name_;
    copy->cols_ = cols_;
    copy->dims_ = dims_;
    copy->cellSize_ = cellSize_;
    copy->buckets_ = buckets_;
    copy->nodes_ = nodes_;
    copy->freeHead_ = freeHead_;
    copy->usedBuckets_ = usedBuckets_;
    copy->entryCount_ = entryCount_;
    return copy;
}

void SpatialIndex::clear(size_t expectedItems) {
    size_t cap = 64;
    const size_t minCap = (expectedItems * 10) / 6 + 16;
    while (cap < minCap) cap <<= 1;
    buckets_.assign(cap, Bucket{});
    nodes_.clear();
    freeHead_ = kInvalidNode;
    usedBuckets_ = 0;
    entryCount_ = 0;
}

int32_t SpatialIndex::cellCoord(double v) const {
    const double scaled = std::floor(v / cellSize_);
    if (scaled <= -2147483000.0) return -2147483000;
    if (scaled >= 2147483000.0) return 2147483000;
    return static_cast<int32_t>(scaled);
}

uint64_t SpatialIndex::hashCell(int32_t cx, int32_t cy, int32_t cz) {
    uint64_t h = mixHash64(static_cast<uint32_t>(cx));
    h = combineHash64(h, static_cast<uint32_t>(cy));
    h = combineHash64(h, static_cast<uint32_t>(cz));
    return h;
}

void SpatialIndex::rehash(size_t newCap) {
    std::vector<Bucket> next(newCap);
    const size_t mask = newCap - 1;
    size_t liveBuckets = 0;
    for (const Bucket& b : buckets_) {
        if (!b.occupied || b.headNode == kInvalidNode) continue;
        size_t pos = static_cast<size_t>(hashCell(b.cx, b.cy, b.cz)) & mask;
        while (next[pos].occupied) pos = (pos + 1) & mask;
        next[pos] = b;
        ++liveBuckets;
    }
    buckets_ = std::move(next);
    usedBuckets_ = liveBuckets;
}

size_t SpatialIndex::findSlot(int32_t cx, int32_t cy, int32_t cz) const {
    if (buckets_.empty()) return 0;
    const size_t mask = buckets_.size() - 1;
    size_t pos = static_cast<size_t>(hashCell(cx, cy, cz)) & mask;
    while (buckets_[pos].occupied &&
           (buckets_[pos].cx != cx || buckets_[pos].cy != cy || buckets_[pos].cz != cz)) {
        pos = (pos + 1) & mask;
    }
    return pos;
}

uint32_t SpatialIndex::findOccupiedHead(int32_t cx, int32_t cy, int32_t cz) const {
    if (buckets_.empty()) return kInvalidNode;
    const size_t pos = findSlot(cx, cy, cz);
    return buckets_[pos].occupied ? buckets_[pos].headNode : kInvalidNode;
}

void SpatialIndex::insert(double x, double y, double z, RowLoc loc) {
    if (!extractCoords(x, y, z, dims_)) return;
    if (buckets_.empty()) clear(64);
    if ((usedBuckets_ + 1) * 10 >= buckets_.size() * 7) {
        size_t liveBuckets = 0;
        for (const Bucket& b : buckets_) {
            if (b.occupied && b.headNode != kInvalidNode) ++liveBuckets;
        }
        const size_t nextCap = ((liveBuckets + 1) * 10 >= buckets_.size() * 6)
                                   ? (buckets_.size() * 2)
                                   : buckets_.size();
        rehash(nextCap);
    }
    const int32_t cx = cellCoord(x);
    const int32_t cy = cellCoord(y);
    const int32_t cz = (dims_ >= 3) ? cellCoord(z) : 0;
    const size_t pos = findSlot(cx, cy, cz);
    Bucket& b = buckets_[pos];
    uint32_t nodeIdx = kInvalidNode;
    if (freeHead_ != kInvalidNode) {
        nodeIdx = freeHead_;
        freeHead_ = nodes_[nodeIdx].next;
    } else {
        nodeIdx = static_cast<uint32_t>(nodes_.size());
        nodes_.emplace_back();
    }
    nodes_[nodeIdx] = Node{loc, x, y, z, kInvalidNode};
    if (!b.occupied) {
        b.occupied = true;
        b.cx = cx;
        b.cy = cy;
        b.cz = cz;
        b.headNode = nodeIdx;
        ++usedBuckets_;
    } else {
        nodes_[nodeIdx].next = b.headNode;
        b.headNode = nodeIdx;
    }
    ++entryCount_;
}

bool SpatialIndex::erase(double x, double y, double z, RowLoc loc) {
    if (!extractCoords(x, y, z, dims_) || buckets_.empty()) return false;
    const int32_t cx = cellCoord(x);
    const int32_t cy = cellCoord(y);
    const int32_t cz = (dims_ >= 3) ? cellCoord(z) : 0;
    const size_t pos = findSlot(cx, cy, cz);
    Bucket& b = buckets_[pos];
    if (!b.occupied) return false;
    uint32_t prev = kInvalidNode;
    uint32_t cur = b.headNode;
    while (cur != kInvalidNode) {
        if (locEq(nodes_[cur].loc, loc)) {
            if (prev == kInvalidNode) b.headNode = nodes_[cur].next;
            else nodes_[prev].next = nodes_[cur].next;
            nodes_[cur].next = freeHead_;
            freeHead_ = cur;
            --entryCount_;
            return true;
        }
        prev = cur;
        cur = nodes_[cur].next;
    }
    return false;
}

void SpatialIndex::queryAABB(double minX, double minY, double minZ,
                             double maxX, double maxY, double maxZ,
                             std::vector<RowLoc>& out) const {
    if (entryCount_ == 0 || minX > maxX || minY > maxY || (dims_ >= 3 && minZ > maxZ)) return;
    const int32_t x0 = cellCoord(minX), x1 = cellCoord(maxX);
    const int32_t y0 = cellCoord(minY), y1 = cellCoord(maxY);
    const int32_t z0 = (dims_ >= 3) ? cellCoord(minZ) : 0;
    const int32_t z1 = (dims_ >= 3) ? cellCoord(maxZ) : 0;
    const uint64_t span = static_cast<uint64_t>(x1 - x0 + 1) *
                          static_cast<uint64_t>(y1 - y0 + 1) *
                          static_cast<uint64_t>(z1 - z0 + 1);

    auto testNode = [&](const Node& n) {
        if (n.x < minX || n.x > maxX || n.y < minY || n.y > maxY) return;
        if (dims_ >= 3 && (n.z < minZ || n.z > maxZ)) return;
        out.push_back(n.loc);
    };

    if (span > 16384 || span > usedBuckets_ * 2ULL) {
        for (const Bucket& b : buckets_) {
            if (!b.occupied) continue;
            if (b.cx < x0 || b.cx > x1 || b.cy < y0 || b.cy > y1 ||
                b.cz < z0 || b.cz > z1) {
                continue;
            }
            for (uint32_t cur = b.headNode; cur != kInvalidNode; cur = nodes_[cur].next) {
                testNode(nodes_[cur]);
            }
        }
        return;
    }

    for (int32_t gz = z0; gz <= z1; ++gz) {
        for (int32_t gy = y0; gy <= y1; ++gy) {
            for (int32_t gx = x0; gx <= x1; ++gx) {
                uint32_t cur = findOccupiedHead(gx, gy, gz);
                while (cur != kInvalidNode) {
                    testNode(nodes_[cur]);
                    cur = nodes_[cur].next;
                }
            }
        }
    }
}

void SpatialIndex::queryRadius(double cx, double cy, double cz, double radius,
                               std::vector<RowLoc>& out) const {
    if (radius < 0.0 || entryCount_ == 0 || !std::isfinite(radius)) return;
    const double r2 = radius * radius;
    const double minX = cx - radius, maxX = cx + radius;
    const double minY = cy - radius, maxY = cy + radius;
    const double minZ = (dims_ >= 3) ? (cz - radius) : 0.0;
    const double maxZ = (dims_ >= 3) ? (cz + radius) : 0.0;

    std::vector<RowLoc> candidates;
    const int32_t x0 = cellCoord(minX), x1 = cellCoord(maxX);
    const int32_t y0 = cellCoord(minY), y1 = cellCoord(maxY);
    const int32_t z0 = (dims_ >= 3) ? cellCoord(minZ) : 0;
    const int32_t z1 = (dims_ >= 3) ? cellCoord(maxZ) : 0;
    const uint64_t span = static_cast<uint64_t>(x1 - x0 + 1) *
                          static_cast<uint64_t>(y1 - y0 + 1) *
                          static_cast<uint64_t>(z1 - z0 + 1);

    auto checkNode = [&](const Node& n) {
        const double dx = n.x - cx;
        const double dy = n.y - cy;
        const double dz = (dims_ >= 3) ? (n.z - cz) : 0.0;
        if (dx * dx + dy * dy + dz * dz <= r2) {
            out.push_back(n.loc);
        }
    };

    if (span > 16384 || span > usedBuckets_ * 2ULL) {
        for (const Bucket& b : buckets_) {
            if (!b.occupied) continue;
            if (b.cx < x0 || b.cx > x1 || b.cy < y0 || b.cy > y1 ||
                b.cz < z0 || b.cz > z1) {
                continue;
            }
            for (uint32_t cur = b.headNode; cur != kInvalidNode; cur = nodes_[cur].next) {
                checkNode(nodes_[cur]);
            }
        }
        return;
    }

    for (int32_t gz = z0; gz <= z1; ++gz) {
        for (int32_t gy = y0; gy <= y1; ++gy) {
            for (int32_t gx = x0; gx <= x1; ++gx) {
                uint32_t cur = findOccupiedHead(gx, gy, gz);
                while (cur != kInvalidNode) {
                    checkNode(nodes_[cur]);
                    cur = nodes_[cur].next;
                }
            }
        }
    }
}

void SpatialIndex::queryNearest(double cx, double cy, double cz, size_t k, double maxRadius,
                                std::vector<std::pair<RowLoc, double>>& out) const {
    out.clear();
    if (k == 0 || entryCount_ == 0 || !std::isfinite(cx) || !std::isfinite(cy)) return;
    if (dims_ >= 3 && !std::isfinite(cz)) return;

    struct Cand {
        double dist2;
        RowLoc loc;
        bool operator<(const Cand& o) const { return dist2 < o.dist2; }
    };
    std::priority_queue<Cand> heap;
    const bool hasMaxR = std::isfinite(maxRadius) && maxRadius >= 0.0;
    const double maxR2 = hasMaxR ? maxRadius * maxRadius : std::numeric_limits<double>::infinity();

    auto consider = [&](const Node& n) {
        const double dx = n.x - cx;
        const double dy = n.y - cy;
        const double dz = (dims_ >= 3) ? (n.z - cz) : 0.0;
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > maxR2) return;
        if (heap.size() < k) {
            heap.push(Cand{d2, n.loc});
        } else if (d2 < heap.top().dist2) {
            heap.pop();
            heap.push(Cand{d2, n.loc});
        }
    };

    const int32_t gcx = cellCoord(cx);
    const int32_t gcy = cellCoord(cy);
    const int32_t gcz = (dims_ >= 3) ? cellCoord(cz) : 0;
    const int32_t maxRing = hasMaxR
                                ? static_cast<int32_t>(std::ceil(maxRadius / cellSize_)) + 1
                                : 16;

    // Expand grid rings until heap kth distance fits inside next ring's minimum box distance.
    int32_t ring = 0;
    bool provedOptimal = false;
    while (ring <= maxRing && ring <= 32) {
        const int32_t zLo = (dims_ >= 3) ? (gcz - ring) : 0;
        const int32_t zHi = (dims_ >= 3) ? (gcz + ring) : 0;
        for (int32_t gz = zLo; gz <= zHi; ++gz) {
            for (int32_t gy = gcy - ring; gy <= gcy + ring; ++gy) {
                for (int32_t gx = gcx - ring; gx <= gcx + ring; ++gx) {
                    const bool onShell =
                        std::abs(gx - gcx) == ring || std::abs(gy - gcy) == ring ||
                        (dims_ >= 3 && std::abs(gz - gcz) == ring);
                    if (!onShell) continue;
                    uint32_t cur = findOccupiedHead(gx, gy, gz);
                    while (cur != kInvalidNode) {
                        consider(nodes_[cur]);
                        cur = nodes_[cur].next;
                    }
                }
            }
        }
        if (heap.size() == k) {
            const double minNextDist = std::max(0.0, static_cast<double>(ring) * cellSize_ - cellSize_);
            if (heap.top().dist2 <= minNextDist * minNextDist) {
                provedOptimal = true;
                break;
            }
        }
        ++ring;
    }
    const bool scannedAll = !provedOptimal && (!hasMaxR || ring <= maxRing);

    if (scannedAll) {
        while (!heap.empty()) heap.pop();
        for (const Bucket& b : buckets_) {
            if (!b.occupied) continue;
            for (uint32_t cur = b.headNode; cur != kInvalidNode; cur = nodes_[cur].next) {
                consider(nodes_[cur]);
            }
        }
    }

    out.reserve(heap.size());
    while (!heap.empty()) {
        const Cand c = heap.top();
        heap.pop();
        out.emplace_back(c.loc, std::sqrt(c.dist2));
    }
    std::reverse(out.begin(), out.end());
}

}  // namespace bro::table
