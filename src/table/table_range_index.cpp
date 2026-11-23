#include "table/table_range_index.h"

#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstring>

namespace bro::table {

namespace {

bool locLess(RowLoc a, RowLoc b) {
    return a.chunkIdx != b.chunkIdx ? (a.chunkIdx < b.chunkIdx) : (a.rowIdx < b.rowIdx);
}

bool locEq(RowLoc a, RowLoc b) {
    return a.chunkIdx == b.chunkIdx && a.rowIdx == b.rowIdx;
}

}  // namespace

RangeIndex::RangeIndex(size_t colIdx, ColType colType)
    : colIdx_(colIdx), colType_(colType) {}

std::shared_ptr<RangeIndex> RangeIndex::createMappedNumeric(
    size_t colIdx, ColType colType,
    const NumEntry* entries, size_t count,
    std::shared_ptr<const void> owner) {
    auto idx = std::make_shared<RangeIndex>();
    idx->colIdx_ = colIdx;
    idx->colType_ = colType;
    idx->sorted_ = true;
    idx->mappedOwner_ = std::move(owner);
    idx->mappedNum_ = entries;
    idx->mappedNumCount_ = count;
    return idx;
}

std::shared_ptr<RangeIndex> RangeIndex::createMappedString(
    size_t colIdx, ColType colType,
    const MappedStrEntry* entries, size_t count,
    const char* blob, size_t blobBytes,
    std::shared_ptr<const void> owner) {
    auto idx = std::make_shared<RangeIndex>();
    idx->colIdx_ = colIdx;
    idx->colType_ = colType;
    idx->sorted_ = true;
    idx->mappedOwner_ = std::move(owner);
    idx->mappedStr_ = entries;
    idx->mappedStrCount_ = count;
    idx->mappedStrBlob_ = blob;
    idx->mappedStrBlobBytes_ = blobBytes;
    return idx;
}

void RangeIndex::ensureOwned() {
    if (!mappedOwner_) return;
    if (isStringOrdered()) {
        strEntries_.clear();
        if (mappedStr_ && mappedStrCount_ > 0) {
            strEntries_.reserve(mappedStrCount_);
            for (size_t i = 0; i < mappedStrCount_; ++i) {
                std::string_view sv = strViewAt(i);
                strEntries_.push_back(StrEntry{std::string(sv), mappedStr_[i].loc});
            }
        }
    } else {
        if (mappedNum_ && mappedNumCount_ > 0) {
            numEntries_.assign(mappedNum_, mappedNum_ + mappedNumCount_);
        } else {
            numEntries_.clear();
        }
    }
    mappedOwner_.reset();
    mappedNum_ = nullptr;
    mappedNumCount_ = 0;
    mappedStr_ = nullptr;
    mappedStrCount_ = 0;
    mappedStrBlob_ = nullptr;
    mappedStrBlobBytes_ = 0;
    sorted_ = true;
}

std::shared_ptr<RangeIndex> RangeIndex::clone() const {
    sortIfNeeded();
    auto copy = std::make_shared<RangeIndex>();
    copy->colIdx_ = colIdx_;
    copy->colType_ = colType_;
    copy->sorted_ = true;
    if (mappedOwner_) {
        if (isStringOrdered()) {
            copy->strEntries_.reserve(mappedStrCount_);
            for (size_t i = 0; i < mappedStrCount_; ++i) {
                copy->strEntries_.push_back(StrEntry{std::string(strViewAt(i)), mappedStr_[i].loc});
            }
        } else if (mappedNum_ && mappedNumCount_ > 0) {
            copy->numEntries_.assign(mappedNum_, mappedNum_ + mappedNumCount_);
        }
        return copy;
    }
    copy->numEntries_ = numEntries_;
    copy->strEntries_ = strEntries_;
    return copy;
}

void RangeIndex::clear() {
    mappedOwner_.reset();
    mappedNum_ = nullptr;
    mappedNumCount_ = 0;
    mappedStr_ = nullptr;
    mappedStrCount_ = 0;
    mappedStrBlob_ = nullptr;
    mappedStrBlobBytes_ = 0;
    numEntries_.clear();
    strEntries_.clear();
    sorted_ = true;
}

std::string_view RangeIndex::strViewAt(size_t i) const {
    if (mappedOwner_) {
        const MappedStrEntry& e = mappedStr_[i];
        const uint64_t end = static_cast<uint64_t>(e.offset) + e.len;
        if (!mappedStrBlob_ || end > mappedStrBlobBytes_) return {};
        return std::string_view(mappedStrBlob_ + e.offset, e.len);
    }
    return strEntries_[i].key;
}

RowLoc RangeIndex::strLocAt(size_t i) const {
    return mappedOwner_ ? mappedStr_[i].loc : strEntries_[i].loc;
}

bool RangeIndex::makeNumEntry(ColType colType, const ScalarValue& v, RowLoc loc,
                              NumEntry& out) {
    if (v.isNull) return false;
    out = NumEntry{};
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
        } else {
            loLim = -9223372036854775808.0;
            hiLim = 9223372036854775808.0;
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
    ensureOwned();
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
    ensureOwned();
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
    if (mappedOwner_ || sorted_) return;
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
        const size_t n = strCount();
        if (n == 0) return;
        auto getText = [&](const ScalarValue& s) -> std::string {
            if (s.type == ColType::Dict && s.str.empty() && dict) {
                return dict->get(static_cast<uint32_t>(s.u64));
            }
            return s.str;
        };
        auto findLower = [&](size_t l, size_t r, std::string_view key, bool strictLt) {
            while (l < r) {
                const size_t mid = l + ((r - l) >> 1);
                const std::string_view mv = strViewAt(mid);
                const bool goRight = strictLt ? (mv < key) : (mv <= key);
                if (goRight) l = mid + 1;
                else r = mid;
            }
            return l;
        };
        size_t first = 0;
        size_t last = n;
        if (lo) {
            std::string loStr = getText(*lo);
            first = findLower(first, last, loStr, incLo);
        }
        if (hi && first < last) {
            std::string hiStr = getText(*hi);
            last = findLower(first, last, hiStr, !incHi);
        }
        out.reserve(out.size() + (last - first));
        for (size_t i = first; i < last; ++i) {
            out.push_back(strLocAt(i));
        }
        return;
    }

    const NumEntry* begin = numData();
    const size_t count = numCount();
    if (!begin || count == 0) return;
    const NumEntry* first = begin;
    const NumEntry* last = begin + count;
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
    out.reserve(out.size() + static_cast<size_t>(last - first));
    for (const NumEntry* it = first; it < last; ++it) {
        out.push_back(it->loc);
    }
}

void RangeIndex::forEachOrdered(bool descending,
                                const std::function<bool(RowLoc)>& visit) const {
    sortIfNeeded();
    if (isStringOrdered()) {
        const size_t n = strCount();
        if (!descending) {
            for (size_t i = 0; i < n; ++i) {
                if (!visit(strLocAt(i))) return;
            }
        } else {
            for (size_t i = n; i > 0; --i) {
                if (!visit(strLocAt(i - 1))) return;
            }
        }
        return;
    }
    const NumEntry* ptr = numData();
    const size_t n = numCount();
    if (!ptr || n == 0) return;
    if (!descending) {
        for (size_t i = 0; i < n; ++i) {
            if (!visit(ptr[i].loc)) return;
        }
    } else {
        for (size_t i = n; i > 0; --i) {
            if (!visit(ptr[i - 1].loc)) return;
        }
    }
}

}  // namespace bro::table
