#include "table/table_ingest_csv.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <unordered_set>

namespace bro::table {

namespace {

constexpr size_t kCsvStreamBuf = 1024u * 1024u;  // 1 MiB fixed streaming read chunk
constexpr size_t kMaxCsvFieldBytes = 16u * 1024u * 1024u;

bool isNullToken(std::string_view sv) {
    if (sv.empty()) return true;
    if (sv == "NA" || sv == "N/A" || sv == "na" || sv == "null" || sv == "NULL" ||
        sv == "NaN" || sv == "nan" || sv == "None" || sv == "\\N") {
        return true;
    }
    return false;
}

bool parseBoolToken(std::string_view sv, bool& out) {
    if (sv == "true" || sv == "TRUE" || sv == "True" || sv == "1") {
        out = true;
        return true;
    }
    if (sv == "false" || sv == "FALSE" || sv == "False" || sv == "0") {
        out = false;
        return true;
    }
    return false;
}

bool parseI64Token(std::string_view sv, int64_t& out) {
    if (sv.empty()) return false;
    const char* begin = sv.data();
    const char* end = begin + sv.size();
    auto res = std::from_chars(begin, end, out, 10);
    return res.ec == std::errc{} && res.ptr == end;
}

bool parseU32Token(std::string_view sv, uint32_t& out) {
    if (sv.empty()) return false;
    const char* begin = sv.data();
    const char* end = begin + sv.size();
    auto res = std::from_chars(begin, end, out, 10);
    return res.ec == std::errc{} && res.ptr == end;
}

bool parseF64Token(std::string_view sv, double& out) {
    if (sv.empty()) return false;
    std::string tmp(sv);
    char* endPtr = nullptr;
    errno = 0;
    out = std::strtod(tmp.c_str(), &endPtr);
    if (errno == ERANGE && out == 0.0) return false;
    return endPtr == tmp.c_str() + tmp.size();
}

enum class CandidateKind : uint8_t { Unknown = 0, Bool, I32, I64, F64, String };

struct ColInference {
    CandidateKind kind = CandidateKind::Unknown;
    std::unordered_set<std::string> distinct;
    size_t nonNullCount = 0;
    bool distinctCapped = false;

    void observe(std::string_view sv, size_t dictLimit) {
        if (isNullToken(sv)) return;
        ++nonNullCount;

        if (!distinctCapped && kind != CandidateKind::I64 && kind != CandidateKind::F64) {
            if (distinct.size() <= dictLimit) {
                distinct.emplace(sv);
            }
            if (distinct.size() > dictLimit) {
                distinctCapped = true;
                distinct.clear();
            }
        }

        if (kind == CandidateKind::String) return;

        bool bVal = false;
        int64_t iVal = 0;
        double dVal = 0.0;

        if (kind == CandidateKind::Unknown) {
            // Prefer exact integer detection before Bool for "0"/"1" so integer columns
            // whose first sample happens to be 0 or 1 widen cleanly without losing type.
            if (sv == "true" || sv == "false" || sv == "TRUE" || sv == "FALSE" ||
                sv == "True" || sv == "False") {
                kind = CandidateKind::Bool;
            } else if (parseI64Token(sv, iVal)) {
                kind = (iVal >= INT32_MIN && iVal <= INT32_MAX) ? CandidateKind::I32 : CandidateKind::I64;
            } else if (parseF64Token(sv, dVal)) {
                kind = CandidateKind::F64;
            } else {
                kind = CandidateKind::String;
            }
            return;
        }

        if (kind == CandidateKind::Bool) {
            if (parseBoolToken(sv, bVal)) return;
            kind = CandidateKind::I32;
        }
        if (kind == CandidateKind::I32) {
            if (parseI64Token(sv, iVal)) {
                if (iVal < INT32_MIN || iVal > INT32_MAX) kind = CandidateKind::I64;
                return;
            }
            kind = CandidateKind::F64;
        }
        if (kind == CandidateKind::I64) {
            if (parseI64Token(sv, iVal)) return;
            kind = CandidateKind::F64;
        }
        if (kind == CandidateKind::F64) {
            if (parseF64Token(sv, dVal)) return;
            kind = CandidateKind::String;
        }
    }

    ColType finalize(size_t dictLimit) const {
        switch (kind) {
            case CandidateKind::Unknown:
            case CandidateKind::String: {
                if (nonNullCount > 0 && !distinctCapped && distinct.size() <= dictLimit &&
                    distinct.size() <= std::max<size_t>(16, nonNullCount / 2)) {
                    return ColType::Dict;
                }
                return ColType::String;
            }
            case CandidateKind::Bool: return ColType::Bool;
            case CandidateKind::I32: return ColType::I32;
            case CandidateKind::I64: return ColType::I64;
            case CandidateKind::F64: return ColType::F64;
        }
        return ColType::String;
    }
};

class CsvRecordParser {
public:
    CsvRecordParser(char delim, char quote) : delim_(delim), quote_(quote) {}

    template <typename RowCallback>
    bool feed(const char* data, size_t len, bool isEof, RowCallback&& onRow, std::string* err) {
        for (size_t i = 0; i < len; ++i) {
            const char ch = data[i];
            if (pendingCr_) {
                pendingCr_ = false;
                if (ch == '\n') continue;
            }

            if (inQuotes_) {
                if (quoteJustClosed_) {
                    quoteJustClosed_ = false;
                    if (ch == quote_) {
                        if (!appendChar(quote_, err)) return false;
                        continue;
                    }
                    inQuotes_ = false;
                    // Fall through cleanly to normal delimiter/EOL handling below.
                } else {
                    if (ch == quote_) {
                        quoteJustClosed_ = true;
                    } else {
                        if (!appendChar(ch, err)) return false;
                    }
                    continue;
                }
            }

            if (ch == quote_ && fieldBuf_.empty()) {
                inQuotes_ = true;
                quoteJustClosed_ = false;
            } else if (ch == delim_) {
                curRow_.push_back(std::move(fieldBuf_));
                fieldBuf_.clear();
            } else if (ch == '\r' || ch == '\n') {
                if (ch == '\r') pendingCr_ = true;
                curRow_.push_back(std::move(fieldBuf_));
                fieldBuf_.clear();
                onRow(curRow_);
                curRow_.clear();
            } else {
                if (!appendChar(ch, err)) return false;
            }
        }

        if (isEof) {
            if (inQuotes_ && !quoteJustClosed_) {
                // Close dangling EOF quote gracefully so slightly truncated exports still parse.
                inQuotes_ = false;
            }
            quoteJustClosed_ = false;
            if (!fieldBuf_.empty() || !curRow_.empty()) {
                curRow_.push_back(std::move(fieldBuf_));
                fieldBuf_.clear();
                onRow(curRow_);
                curRow_.clear();
            }
        }
        return true;
    }

private:
    bool appendChar(char c, std::string* err) {
        if (fieldBuf_.size() >= kMaxCsvFieldBytes) {
            if (err) *err = "CSV field exceeds maximum size limit (16 MiB)";
            return false;
        }
        fieldBuf_.push_back(c);
        return true;
    }

    char delim_;
    char quote_;
    bool inQuotes_ = false;
    bool quoteJustClosed_ = false;
    bool pendingCr_ = false;
    std::string fieldBuf_;
    std::vector<std::string> curRow_;
};

class CsvTableSink {
public:
    CsvTableSink(const CsvIngestOptions& opts) : opts_(opts) {
        if (!opts_.explicitSchema.empty()) {
            schema_ = opts_.explicitSchema;
            hasSchema_ = true;
            table_ = std::make_shared<Table>(schema_);
            allocateBatchBuffers();
        }
    }

    bool wantsMoreRows() const {
        return opts_.maxRows == 0 || totalDataRows_ < opts_.maxRows;
    }

    void acceptRow(const std::vector<std::string>& row) {
        if (!wantsMoreRows()) return;

        if (rawLineIdx_ < opts_.skipRows) {
            ++rawLineIdx_;
            return;
        }
        ++rawLineIdx_;

        // Skip a single completely empty trailing blank line.
        if (row.size() == 1 && row[0].empty() && totalDataRows_ == 0 && headerConsumed_) return;

        if (!headerConsumed_) {
            headerConsumed_ = true;
            if (opts_.hasHeader) {
                if (!hasSchema_) {
                    headers_ = row;
                    ensureUniqueHeaders(headers_);
                }
                return;
            }
            if (!hasSchema_) {
                headers_.resize(row.size());
                for (size_t c = 0; c < row.size(); ++c) {
                    headers_[c] = "col_" + std::to_string(c);
                }
            }
        }

        if (!hasSchema_) {
            sampleRows_.push_back(row);
            if (sampleRows_.size() >= opts_.inferSampleRows ||
                (opts_.maxRows > 0 && sampleRows_.size() >= opts_.maxRows)) {
                finalizeInferredSchemaAndFlush();
            }
            return;
        }

        pushTypedRow(row);
    }

    std::shared_ptr<Table> finish() {
        if (!hasSchema_) {
            finalizeInferredSchemaAndFlush();
        }
        flushBatch();
        return table_ ? table_ : std::make_shared<Table>();
    }

private:
    static void ensureUniqueHeaders(std::vector<std::string>& names) {
        std::unordered_set<std::string> used;
        for (size_t i = 0; i < names.size(); ++i) {
            std::string base = names[i].empty() ? ("col_" + std::to_string(i)) : names[i];
            std::string cand = base;
            size_t suffix = 1;
            while (!used.insert(cand).second) {
                cand = base + "_" + std::to_string(suffix++);
            }
            names[i] = std::move(cand);
        }
    }

    void finalizeInferredSchemaAndFlush() {
        if (hasSchema_) return;
        hasSchema_ = true;
        size_t nCols = headers_.size();
        for (const auto& r : sampleRows_) nCols = std::max(nCols, r.size());
        if (nCols == 0) {
            table_ = std::make_shared<Table>();
            return;
        }
        while (headers_.size() < nCols) headers_.push_back("col_" + std::to_string(headers_.size()));
        ensureUniqueHeaders(headers_);

        std::vector<ColInference> inf(nCols);
        for (const auto& r : sampleRows_) {
            for (size_t c = 0; c < nCols; ++c) {
                std::string_view sv = c < r.size() ? std::string_view(r[c]) : std::string_view{};
                inf[c].observe(sv, opts_.dictMaxCardinality);
            }
        }
        schema_.clear();
        schema_.reserve(nCols);
        for (size_t c = 0; c < nCols; ++c) {
            schema_.push_back(ColumnDef{headers_[c], inf[c].finalize(opts_.dictMaxCardinality)});
        }
        table_ = std::make_shared<Table>(schema_);
        allocateBatchBuffers();

        for (const auto& r : sampleRows_) {
            if (!wantsMoreRows()) break;
            pushTypedRow(r);
        }
        sampleRows_.clear();
        sampleRows_.shrink_to_fit();
    }

    void allocateBatchBuffers() {
        const size_t nCols = schema_.size();
        colFixed_.resize(nCols);
        colNulls_.resize(nCols);
        colStrings_.resize(nCols);
        for (size_t c = 0; c < nCols; ++c) {
            const ColType ct = schema_[c].type;
            if (ct == ColType::String) {
                colStrings_[c].resize(kChunkRows);
            } else {
                colFixed_[c].assign(kChunkRows * colTypeElemSize(ct), 0);
            }
            colNulls_[c].assign(kChunkRows, 0);
        }
        batchCount_ = 0;
    }

    void pushTypedRow(const std::vector<std::string>& row) {
        if (!table_ || schema_.empty()) return;
        const size_t nCols = schema_.size();
        const uint32_t idx = batchCount_;

        for (size_t c = 0; c < nCols; ++c) {
            const ColType ct = schema_[c].type;
            std::string_view sv = (c < row.size()) ? std::string_view(row[c]) : std::string_view{};
            if (isNullToken(sv)) {
                colNulls_[c][idx] = 1;
                if (ct == ColType::String) {
                    colStrings_[c][idx].clear();
                } else {
                    const size_t es = colTypeElemSize(ct);
                    std::memset(colFixed_[c].data() + static_cast<size_t>(idx) * es, 0, es);
                }
                continue;
            }
            colNulls_[c][idx] = 0;
            bool ok = true;
            switch (ct) {
                case ColType::Bool: {
                    bool bv = false;
                    ok = parseBoolToken(sv, bv);
                    if (!ok) {
                        double dv = 0.0;
                        ok = parseF64Token(sv, dv);
                        bv = ok && (dv != 0.0);
                    }
                    colFixed_[c][idx] = (ok && bv) ? 1 : 0;
                    break;
                }
                case ColType::I32: {
                    int64_t iv = 0;
                    ok = parseI64Token(sv, iv);
                    int32_t v = ok ? static_cast<int32_t>(iv) : 0;
                    std::memcpy(colFixed_[c].data() + static_cast<size_t>(idx) * 4, &v, 4);
                    break;
                }
                case ColType::U32: {
                    uint32_t uv = 0;
                    ok = parseU32Token(sv, uv);
                    std::memcpy(colFixed_[c].data() + static_cast<size_t>(idx) * 4, &uv, 4);
                    break;
                }
                case ColType::I64: {
                    int64_t iv = 0;
                    ok = parseI64Token(sv, iv);
                    std::memcpy(colFixed_[c].data() + static_cast<size_t>(idx) * 8, &iv, 8);
                    break;
                }
                case ColType::F32: {
                    double dv = 0.0;
                    ok = parseF64Token(sv, dv);
                    float fv = ok ? static_cast<float>(dv) : 0.0f;
                    std::memcpy(colFixed_[c].data() + static_cast<size_t>(idx) * 4, &fv, 4);
                    break;
                }
                case ColType::F64: {
                    double dv = 0.0;
                    ok = parseF64Token(sv, dv);
                    if (!ok) dv = 0.0;
                    std::memcpy(colFixed_[c].data() + static_cast<size_t>(idx) * 8, &dv, 8);
                    break;
                }
                case ColType::Dict: {
                    uint32_t id = table_->internString(sv);
                    std::memcpy(colFixed_[c].data() + static_cast<size_t>(idx) * 4, &id, 4);
                    break;
                }
                case ColType::String: {
                    colStrings_[c][idx].assign(sv);
                    break;
                }
            }
            if (!ok) {
                colNulls_[c][idx] = 1;
            }
        }

        ++batchCount_;
        ++totalDataRows_;
        if (batchCount_ >= kChunkRows) flushBatch();
    }

    void flushBatch() {
        if (batchCount_ == 0 || !table_) return;
        const size_t nCols = schema_.size();
        const uint32_t count = batchCount_;
        batchCount_ = 0;

        // If schema has String columns or any nulls, keep chunk boundaries exact by taking
        // slices that fit in the current tail chunk.
        const size_t startChunk = table_->chunkCount() > 0 ? table_->chunkCount() - 1 : 0;
        const uint32_t startRowInTail =
            (table_->chunkCount() > 0) ? table_->chunk(startChunk).rowCount : 0;

        std::vector<const void*> ptrs(nCols, nullptr);
        for (size_t c = 0; c < nCols; ++c) {
            if (schema_[c].type != ColType::String) {
                ptrs[c] = colFixed_[c].data();
            }
        }
        table_->appendTypedBatch(count, ptrs.data());

        // Populate String spans and any non-null reset / null bits on newly appended rows.
        uint32_t localOffset = 0;
        size_t ci = startChunk;
        uint32_t rowCursor = startRowInTail;
        while (localOffset < count && ci < table_->chunkCount()) {
            TableChunk& ch = table_->chunkMut(ci);
            if (rowCursor >= ch.rowCount) {
                ++ci;
                rowCursor = 0;
                continue;
            }
            const uint32_t inChunk = std::min(count - localOffset, ch.rowCount - rowCursor);
            for (size_t c = 0; c < nCols; ++c) {
                auto& colPtr = ch.mutableColumn(c);
                const bool isStr = (schema_[c].type == ColType::String);
                for (uint32_t i = 0; i < inChunk; ++i) {
                    const uint32_t srcIdx = localOffset + i;
                    const uint32_t dstRow = rowCursor + i;
                    if (colNulls_[c][srcIdx]) {
                        colPtr->setNull(dstRow, true);
                    } else if (isStr) {
                        colPtr->setNull(dstRow, false);
                        colPtr->setString(dstRow, colStrings_[c][srcIdx]);
                    }
                }
            }
            localOffset += inChunk;
            ++ci;
            rowCursor = 0;
        }
    }

    CsvIngestOptions opts_;
    std::vector<ColumnDef> schema_;
    std::vector<std::string> headers_;
    std::vector<std::vector<std::string>> sampleRows_;
    bool headerConsumed_ = false;
    bool hasSchema_ = false;
    size_t rawLineIdx_ = 0;
    size_t totalDataRows_ = 0;
    std::shared_ptr<Table> table_;

    std::vector<std::vector<uint8_t>> colFixed_;
    std::vector<std::vector<uint8_t>> colNulls_;
    std::vector<std::vector<std::string>> colStrings_;
    uint32_t batchCount_ = 0;
};

}  // namespace

std::shared_ptr<Table> ingestCsvFromMemory(const char* data, size_t size,
                                           const CsvIngestOptions& opts,
                                           std::string* outError) {
    CsvTableSink sink(opts);
    CsvRecordParser parser(opts.delimiter, opts.quote);
    std::string parseErr;
    if (size > 0 && !data) {
        if (outError) *outError = "ingestCsvFromMemory: null data pointer";
        return nullptr;
    }
    if (!parser.feed(data ? data : "", size, true,
                     [&](const std::vector<std::string>& row) { sink.acceptRow(row); },
                     &parseErr)) {
        if (outError) *outError = parseErr;
        return nullptr;
    }
    return sink.finish();
}

std::shared_ptr<Table> ingestCsvFromFile(const std::string& path,
                                         const CsvIngestOptions& opts,
                                         std::string* outError) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (outError) *outError = "ingestCsvFromFile: failed to open CSV file: " + path;
        return nullptr;
    }

    std::vector<char> buf(kCsvStreamBuf);
    CsvTableSink sink(opts);
    CsvRecordParser parser(opts.delimiter, opts.quote);
    std::string parseErr;
    bool strippedBom = false;

    while (in && sink.wantsMoreRows()) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const size_t got = static_cast<size_t>(in.gcount());
        const bool eof = in.eof();
        const char* ptr = buf.data();
        size_t len = got;
        if (!strippedBom && len >= 3 &&
            static_cast<uint8_t>(ptr[0]) == 0xEF &&
            static_cast<uint8_t>(ptr[1]) == 0xBB &&
            static_cast<uint8_t>(ptr[2]) == 0xBF) {
            ptr += 3;
            len -= 3;
        }
        strippedBom = true;

        if (!parser.feed(ptr, len, eof,
                         [&](const std::vector<std::string>& row) { sink.acceptRow(row); },
                         &parseErr)) {
            if (outError) *outError = parseErr;
            return nullptr;
        }
        if (eof) break;
    }
    if (!in.eof()) {
        // Early stop due to maxRows — still finalize cleanly.
    }
    return sink.finish();
}

}  // namespace bro::table
