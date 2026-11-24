#pragma once

#include "table/table_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace bro::table::parquet_detail {

inline constexpr char kParquetMagic[4] = {'P', 'A', 'R', '1'};
inline constexpr uint8_t kCompactBoolTrue = 1;
inline constexpr uint8_t kCompactBoolFalse = 2;
inline constexpr uint8_t kCompactI32 = 5;
inline constexpr uint8_t kCompactI64 = 6;
inline constexpr uint8_t kCompactBinary = 8;
inline constexpr uint8_t kCompactList = 9;
inline constexpr uint8_t kCompactStruct = 12;

enum PqType : int32_t {
    PQ_BOOLEAN = 0,
    PQ_INT32 = 1,
    PQ_INT64 = 2,
    PQ_INT96 = 3,
    PQ_FLOAT = 4,
    PQ_DOUBLE = 5,
    PQ_BYTE_ARRAY = 6,
    PQ_FIXED_LEN_BYTE_ARRAY = 7,
};

enum PqEncoding : int32_t {
    PQ_PLAIN = 0,
    PQ_PLAIN_DICT = 2,
    PQ_RLE = 3,
    PQ_BIT_PACKED = 4,
    PQ_RLE_DICT = 8,
};

enum PqCodec : int32_t {
    PQ_UNCOMPRESSED = 0,
    PQ_SNAPPY = 1,
    PQ_LZ4 = 5,
    PQ_LZ4_RAW = 7,
};

enum PqPageType : int32_t {
    PQ_DATA_PAGE = 0,
    PQ_INDEX_PAGE = 1,
    PQ_DICTIONARY_PAGE = 2,
    PQ_DATA_PAGE_V2 = 3,
};

struct PqSchemaElem {
    bool hasType = false;
    int32_t type = PQ_BYTE_ARRAY;
    int32_t typeLen = 0;
    bool hasRep = false;
    int32_t rep = 0;
    std::string name;
    int32_t numChildren = 0;
    bool hasConverted = false;
    int32_t converted = 0;
};

struct PqColMeta {
    int32_t type = PQ_BYTE_ARRAY;
    int32_t codec = PQ_UNCOMPRESSED;
    int64_t numValues = 0;
    int64_t totalCompressed = 0;
    int64_t dataPageOffset = 0;
    bool hasDictOffset = false;
    int64_t dictPageOffset = 0;
    std::vector<std::string> path;
};

struct PqColChunk {
    int64_t fileOffset = 0;
    PqColMeta meta;
};

struct PqRowGroup {
    std::vector<PqColChunk> columns;
    int64_t numRows = 0;
};

struct PqFileMeta {
    int32_t version = 1;
    std::vector<PqSchemaElem> schema;
    int64_t numRows = 0;
    std::vector<PqRowGroup> rowGroups;
};

struct PqPageHeader {
    int32_t type = 0;
    int32_t uncompressedSize = 0;
    int32_t compressedSize = 0;
    int32_t numValues = 0;
    int32_t encoding = PQ_PLAIN;
    int32_t defEncoding = PQ_RLE;
    int32_t repEncoding = PQ_RLE;
    int32_t numNulls = 0;
    int32_t defBytesLen = 0;
    int32_t repBytesLen = 0;
    bool v2Compressed = true;
};

class CompactWriter {
public:
    std::vector<uint8_t> buf;

    void varU64(uint64_t v);
    void zigI32(int32_t v);
    void zigI64(int64_t v);
    void field(int16_t& prevId, int16_t id, uint8_t type);
    void structEnd();
    void fieldI32(int16_t& prev, int16_t id, int32_t v);
    void fieldI64(int16_t& prev, int16_t id, int64_t v);
    void fieldBin(int16_t& prev, int16_t id, const std::string& s);
    void listHeader(uint32_t count, uint8_t elemType);
};

bool readFileMetaData(const uint8_t* p, size_t n, PqFileMeta& out);
bool readPageHeader(const uint8_t* p, size_t n, PqPageHeader& out, size_t& headerBytes);
bool decodeRleHybrid(const uint8_t* data, size_t len, uint32_t bitWidth, uint32_t count,
                     std::vector<uint32_t>& out, std::string* err);
bool decompressPage(int32_t codec, const uint8_t* src, size_t compLen, size_t uncompLen,
                    std::vector<uint8_t>& out, std::string* err);
void writeDefLevelsV1(std::vector<uint8_t>& out, const std::vector<uint8_t>& isPresent);
int32_t broTypeToParquetPhys(ColType t);
void writeDataPageHeader(CompactWriter& w, int32_t uncompSize, int32_t compSize, int32_t numValues);

}  // namespace bro::table::parquet_detail
