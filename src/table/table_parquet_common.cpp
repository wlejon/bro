#include "table/table_parquet_internal.h"

#include "table/table_codec.h"

#include <algorithm>
#include <cstring>

namespace bro::table::parquet_detail {

namespace {

class CompactReader {
public:
    CompactReader(const uint8_t* p, size_t n) : ptr(p), len(n) {}

    bool ok() const { return ok_; }
    size_t position() const { return pos; }

    uint8_t u8() {
        if (!ok_ || pos >= len) { ok_ = false; return 0; }
        return ptr[pos++];
    }

    uint64_t varU64() {
        uint64_t v = 0;
        uint32_t s = 0;
        while (ok_) {
            if (pos >= len || s >= 64) { ok_ = false; return 0; }
            const uint8_t b = ptr[pos++];
            v |= static_cast<uint64_t>(b & 0x7Fu) << s;
            if ((b & 0x80u) == 0) break;
            s += 7;
        }
        return v;
    }

    uint32_t varU32() { return static_cast<uint32_t>(varU64()); }

    int32_t zigI32() {
        const uint32_t n = varU32();
        return static_cast<int32_t>((n >> 1) ^ (~(n & 1u) + 1u));
    }

    int64_t zigI64() {
        const uint64_t n = varU64();
        return static_cast<int64_t>((n >> 1) ^ (~(n & 1ULL) + 1ULL));
    }

    std::string binary() {
        const uint32_t n = varU32();
        if (!ok_ || n > len - pos) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(ptr + pos), n);
        pos += n;
        return s;
    }

    bool nextField(int16_t& prevId, int16_t& outId, uint8_t& outType) {
        const uint8_t b = u8();
        if (!ok_ || b == 0) return false;
        const uint8_t delta = b >> 4;
        outType = b & 0x0Fu;
        outId = (delta == 0) ? static_cast<int16_t>(zigI32())
                             : static_cast<int16_t>(prevId + static_cast<int16_t>(delta));
        prevId = outId;
        return ok_;
    }

    bool listHeader(uint32_t& count, uint8_t& elemType) {
        const uint8_t b = u8();
        if (!ok_) return false;
        elemType = b & 0x0Fu;
        uint32_t sz = b >> 4;
        if (sz == 15) sz = varU32();
        count = sz;
        return ok_;
    }

    bool skip(uint8_t type, int depth = 0) {
        if (!ok_ || depth > 32) { ok_ = false; return false; }
        switch (type) {
            case kCompactBoolTrue:
            case kCompactBoolFalse:
                return true;
            case 3:
                (void)u8();
                return ok_;
            case 4:
            case kCompactI32:
            case kCompactI64:
                (void)varU64();
                return ok_;
            case 7:
                if (len - pos < 8) { ok_ = false; return false; }
                pos += 8;
                return true;
            case kCompactBinary: {
                const uint32_t n = varU32();
                if (!ok_ || n > len - pos) { ok_ = false; return false; }
                pos += n;
                return true;
            }
            case kCompactList:
            case 10: {
                uint32_t n = 0;
                uint8_t et = 0;
                if (!listHeader(n, et)) return false;
                for (uint32_t i = 0; i < n && ok_; ++i) skip(et, depth + 1);
                return ok_;
            }
            case 11: {
                const uint32_t n = varU32();
                if (!ok_ || n == 0) return ok_;
                const uint8_t kv = u8();
                const uint8_t kt = kv >> 4;
                const uint8_t vt = kv & 0x0Fu;
                for (uint32_t i = 0; i < n && ok_; ++i) {
                    skip(kt, depth + 1);
                    skip(vt, depth + 1);
                }
                return ok_;
            }
            case kCompactStruct: {
                int16_t prev = 0, fid = 0;
                uint8_t ft = 0;
                while (nextField(prev, fid, ft)) {
                    if (!skip(ft, depth + 1)) return false;
                }
                return ok_;
            }
            default:
                ok_ = false;
                return false;
        }
    }

private:
    const uint8_t* ptr = nullptr;
    size_t len = 0;
    size_t pos = 0;
    bool ok_ = true;
};

bool readSchemaElem(CompactReader& r, PqSchemaElem& out) {
    int16_t prev = 0, fid = 0;
    uint8_t ft = 0;
    while (r.nextField(prev, fid, ft)) {
        switch (fid) {
            case 1: out.hasType = true; out.type = r.zigI32(); break;
            case 2: out.typeLen = r.zigI32(); break;
            case 3: out.hasRep = true; out.rep = r.zigI32(); break;
            case 4: out.name = r.binary(); break;
            case 5: out.numChildren = r.zigI32(); break;
            case 6: out.hasConverted = true; out.converted = r.zigI32(); break;
            default: r.skip(ft); break;
        }
    }
    return r.ok();
}

bool readColMeta(CompactReader& r, PqColMeta& out) {
    int16_t prev = 0, fid = 0;
    uint8_t ft = 0;
    while (r.nextField(prev, fid, ft)) {
        switch (fid) {
            case 1: out.type = r.zigI32(); break;
            case 3: {
                uint32_t n = 0;
                uint8_t et = 0;
                if (!r.listHeader(n, et)) return false;
                for (uint32_t i = 0; i < n; ++i) out.path.push_back(r.binary());
                break;
            }
            case 4: out.codec = r.zigI32(); break;
            case 5: out.numValues = r.zigI64(); break;
            case 7: out.totalCompressed = r.zigI64(); break;
            case 9: out.dataPageOffset = r.zigI64(); break;
            case 11: out.hasDictOffset = true; out.dictPageOffset = r.zigI64(); break;
            default: r.skip(ft); break;
        }
    }
    return r.ok();
}

bool readColChunk(CompactReader& r, PqColChunk& out) {
    int16_t prev = 0, fid = 0;
    uint8_t ft = 0;
    while (r.nextField(prev, fid, ft)) {
        if (fid == 2) out.fileOffset = r.zigI64();
        else if (fid == 3) readColMeta(r, out.meta);
        else r.skip(ft);
    }
    return r.ok();
}

bool readRowGroup(CompactReader& r, PqRowGroup& out) {
    int16_t prev = 0, fid = 0;
    uint8_t ft = 0;
    while (r.nextField(prev, fid, ft)) {
        if (fid == 1) {
            uint32_t n = 0;
            uint8_t et = 0;
            if (!r.listHeader(n, et)) return false;
            out.columns.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                if (!readColChunk(r, out.columns[i])) return false;
            }
        } else if (fid == 2 || fid == 3) {
            out.numRows = r.zigI64();
        } else {
            r.skip(ft);
        }
    }
    return r.ok();
}

void writeRleRepeated(std::vector<uint8_t>& dst, uint32_t count, uint32_t value, uint32_t bitWidth) {
    if (count == 0) return;
    uint64_t hdr = static_cast<uint64_t>(count) << 1;
    while (hdr >= 0x80u) {
        dst.push_back(static_cast<uint8_t>((hdr & 0x7Fu) | 0x80u));
        hdr >>= 7;
    }
    dst.push_back(static_cast<uint8_t>(hdr));
    const size_t nBytes = std::max<size_t>(1, (bitWidth + 7u) / 8u);
    for (size_t i = 0; i < nBytes; ++i) {
        dst.push_back(static_cast<uint8_t>((value >> (8u * i)) & 0xFFu));
    }
}

}  // namespace

void CompactWriter::varU64(uint64_t v) {
    while (v >= 0x80u) {
        buf.push_back(static_cast<uint8_t>((v & 0x7Fu) | 0x80u));
        v >>= 7;
    }
    buf.push_back(static_cast<uint8_t>(v));
}

void CompactWriter::zigI32(int32_t v) {
    const uint32_t uv = (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31);
    varU64(uv);
}

void CompactWriter::zigI64(int64_t v) {
    const uint64_t uv = (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
    varU64(uv);
}

void CompactWriter::field(int16_t& prevId, int16_t id, uint8_t type) {
    const int16_t d = id - prevId;
    if (d > 0 && d <= 15) {
        buf.push_back(static_cast<uint8_t>((static_cast<uint8_t>(d) << 4) | (type & 0x0Fu)));
    } else {
        buf.push_back(type & 0x0Fu);
        zigI32(id);
    }
    prevId = id;
}

void CompactWriter::structEnd() { buf.push_back(0); }
void CompactWriter::fieldI32(int16_t& prev, int16_t id, int32_t v) { field(prev, id, kCompactI32); zigI32(v); }
void CompactWriter::fieldI64(int16_t& prev, int16_t id, int64_t v) { field(prev, id, kCompactI64); zigI64(v); }

void CompactWriter::fieldBin(int16_t& prev, int16_t id, const std::string& s) {
    field(prev, id, kCompactBinary);
    varU64(s.size());
    buf.insert(buf.end(), s.begin(), s.end());
}

void CompactWriter::listHeader(uint32_t count, uint8_t elemType) {
    if (count < 15) {
        buf.push_back(static_cast<uint8_t>((count << 4) | (elemType & 0x0Fu)));
    } else {
        buf.push_back(static_cast<uint8_t>(0xF0u | (elemType & 0x0Fu)));
        varU64(count);
    }
}

bool readFileMetaData(const uint8_t* p, size_t n, PqFileMeta& out) {
    CompactReader r(p, n);
    int16_t prev = 0, fid = 0;
    uint8_t ft = 0;
    while (r.nextField(prev, fid, ft)) {
        switch (fid) {
            case 1: out.version = r.zigI32(); break;
            case 2: {
                uint32_t cnt = 0;
                uint8_t et = 0;
                if (!r.listHeader(cnt, et)) return false;
                out.schema.resize(cnt);
                for (uint32_t i = 0; i < cnt; ++i) {
                    if (!readSchemaElem(r, out.schema[i])) return false;
                }
                break;
            }
            case 3: out.numRows = r.zigI64(); break;
            case 4: {
                uint32_t cnt = 0;
                uint8_t et = 0;
                if (!r.listHeader(cnt, et)) return false;
                out.rowGroups.resize(cnt);
                for (uint32_t i = 0; i < cnt; ++i) {
                    if (!readRowGroup(r, out.rowGroups[i])) return false;
                }
                break;
            }
            default: r.skip(ft); break;
        }
    }
    return r.ok();
}

bool readPageHeader(const uint8_t* p, size_t n, PqPageHeader& out, size_t& headerBytes) {
    CompactReader r(p, n);
    int16_t prev = 0, fid = 0;
    uint8_t ft = 0;
    while (r.nextField(prev, fid, ft)) {
        switch (fid) {
            case 1: out.type = r.zigI32(); break;
            case 2: out.uncompressedSize = r.zigI32(); break;
            case 3: out.compressedSize = r.zigI32(); break;
            case 5: {
                int16_t dpPrev = 0, dpid = 0;
                uint8_t dpft = 0;
                while (r.nextField(dpPrev, dpid, dpft)) {
                    if (dpid == 1) out.numValues = r.zigI32();
                    else if (dpid == 2) out.encoding = r.zigI32();
                    else if (dpid == 3) out.defEncoding = r.zigI32();
                    else if (dpid == 4) out.repEncoding = r.zigI32();
                    else r.skip(dpft);
                }
                break;
            }
            case 7: {
                int16_t dpPrev = 0, dpid = 0;
                uint8_t dpft = 0;
                while (r.nextField(dpPrev, dpid, dpft)) {
                    if (dpid == 1) out.numValues = r.zigI32();
                    else if (dpid == 2) out.encoding = r.zigI32();
                    else r.skip(dpft);
                }
                break;
            }
            case 8: {
                int16_t v2Prev = 0, v2id = 0;
                uint8_t v2ft = 0;
                while (r.nextField(v2Prev, v2id, v2ft)) {
                    if (v2id == 1) out.numValues = r.zigI32();
                    else if (v2id == 2) out.numNulls = r.zigI32();
                    else if (v2id == 4) out.encoding = r.zigI32();
                    else if (v2id == 5) out.defBytesLen = r.zigI32();
                    else if (v2id == 6) out.repBytesLen = r.zigI32();
                    else if (v2id == 7) out.v2Compressed = (v2ft == kCompactBoolTrue);
                    else r.skip(v2ft);
                }
                break;
            }
            default: r.skip(ft); break;
        }
    }
    headerBytes = r.position();
    return r.ok();
}

bool decodeRleHybrid(const uint8_t* data, size_t len, uint32_t bitWidth, uint32_t count,
                     std::vector<uint32_t>& out, std::string* err) {
    out.clear();
    out.reserve(count);
    if (count == 0) return true;
    if (bitWidth == 0) {
        out.assign(count, 0u);
        return true;
    }

    size_t pos = 0;
    const size_t valBytes = (bitWidth + 7u) / 8u;
    const uint64_t mask = (bitWidth >= 32) ? 0xFFFFFFFFULL : ((1ULL << bitWidth) - 1ULL);

    while (out.size() < count) {
        uint64_t hdr = 0;
        uint32_t shift = 0;
        while (true) {
            if (pos >= len || shift > 28) {
                if (err) *err = "parquet: truncated RLE header";
                return false;
            }
            const uint8_t b = data[pos++];
            hdr |= static_cast<uint64_t>(b & 0x7Fu) << shift;
            if ((b & 0x80u) == 0) break;
            shift += 7;
        }

        if ((hdr & 1ULL) == 0) {
            const uint64_t run = hdr >> 1;
            if (pos + valBytes > len) {
                if (err) *err = "parquet: truncated RLE value";
                return false;
            }
            uint32_t v = 0;
            for (size_t i = 0; i < valBytes; ++i) {
                v |= static_cast<uint32_t>(data[pos++]) << (8u * i);
            }
            v &= static_cast<uint32_t>(mask);
            const size_t take = static_cast<size_t>(std::min<uint64_t>(run, count - out.size()));
            for (size_t i = 0; i < take; ++i) out.push_back(v);
        } else {
            const uint64_t groups = hdr >> 1;
            const uint64_t nVals = groups * 8ULL;
            const uint64_t nBytes = (nVals * bitWidth + 7ULL) / 8ULL;
            if (nBytes > len - pos) {
                if (err) *err = "parquet: truncated bit-packed RLE run";
                return false;
            }
            const uint8_t* pack = data + pos;
            pos += static_cast<size_t>(nBytes);
            uint64_t bitOff = 0;
            const size_t take = static_cast<size_t>(std::min<uint64_t>(nVals, count - out.size()));
            for (size_t i = 0; i < take; ++i) {
                uint32_t val = 0;
                uint32_t bitsRem = bitWidth;
                uint32_t dstShift = 0;
                uint64_t curBit = bitOff;
                while (bitsRem > 0) {
                    const size_t byteIdx = static_cast<size_t>(curBit >> 3);
                    const uint32_t bitInByte = static_cast<uint32_t>(curBit & 7u);
                    const uint32_t avail = std::min(bitsRem, 8u - bitInByte);
                    const uint32_t chunk = (pack[byteIdx] >> bitInByte) & ((1u << avail) - 1u);
                    val |= (chunk << dstShift);
                    dstShift += avail;
                    curBit += avail;
                    bitsRem -= avail;
                }
                out.push_back(val);
                bitOff += bitWidth;
            }
        }
    }
    return true;
}

bool decompressPage(int32_t codec, const uint8_t* src, size_t compLen, size_t uncompLen,
                    std::vector<uint8_t>& out, std::string* err) {
    if (codec == PQ_UNCOMPRESSED) {
        out.assign(src, src + compLen);
        return true;
    }
    if (codec == PQ_SNAPPY) {
        return snappyDecompress(src, compLen, out, err);
    }
    if (codec == PQ_LZ4_RAW || codec == PQ_LZ4) {
        out.resize(uncompLen);
        return lz4BlockDecompress(src, compLen, out.data(), uncompLen, err);
    }
    if (err) *err = "parquet: unsupported compression codec (" + std::to_string(codec) + ")";
    return false;
}

void writeDefLevelsV1(std::vector<uint8_t>& out, const std::vector<uint8_t>& isPresent) {
    std::vector<uint8_t> rle;
    size_t i = 0;
    while (i < isPresent.size()) {
        size_t j = i + 1;
        while (j < isPresent.size() && isPresent[j] == isPresent[i]) ++j;
        writeRleRepeated(rle, static_cast<uint32_t>(j - i), isPresent[i] ? 1u : 0u, 1u);
        i = j;
    }
    const uint32_t byteLen = static_cast<uint32_t>(rle.size());
    const size_t off = out.size();
    out.resize(off + 4);
    std::memcpy(out.data() + off, &byteLen, 4);
    out.insert(out.end(), rle.begin(), rle.end());
}

int32_t broTypeToParquetPhys(ColType t) {
    switch (t) {
        case ColType::Bool: return PQ_BOOLEAN;
        case ColType::I32: return PQ_INT32;
        case ColType::U32:
        case ColType::I64: return PQ_INT64;
        case ColType::F32: return PQ_FLOAT;
        case ColType::F64: return PQ_DOUBLE;
        case ColType::Dict:
        case ColType::String: return PQ_BYTE_ARRAY;
    }
    return PQ_BYTE_ARRAY;
}

void writeDataPageHeader(CompactWriter& w, int32_t uncompSize, int32_t compSize, int32_t numValues) {
    int16_t prev = 0;
    w.fieldI32(prev, 1, PQ_DATA_PAGE);
    w.fieldI32(prev, 2, uncompSize);
    w.fieldI32(prev, 3, compSize);
    w.field(prev, 5, kCompactStruct);
    int16_t dpPrev = 0;
    w.fieldI32(dpPrev, 1, numValues);
    w.fieldI32(dpPrev, 2, PQ_PLAIN);
    w.fieldI32(dpPrev, 3, PQ_RLE);
    w.fieldI32(dpPrev, 4, PQ_RLE);
    w.structEnd();
    w.structEnd();
}

}  // namespace bro::table::parquet_detail
