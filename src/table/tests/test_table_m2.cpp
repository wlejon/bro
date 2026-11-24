#include "table/table.h"
#include "table/table_codec.h"
#include "table/table_ingest_csv.h"
#include "table/table_ingest_parquet.h"
#include "table/table_io.h"
#include "table/table_io_bundle.h"
#include "table/table_reactive.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace bro::table;

namespace {

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "CHECK FAILED [" << __FILE__ << ":" << __LINE__ << "]: " \
                      << #cond << "\n";                                          \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int testCodecs() {
    // 1. LZ4 repetitive + mixed payload round-trip
    std::vector<uint8_t> src(65536);
    for (size_t i = 0; i < src.size(); ++i) {
        src[i] = static_cast<uint8_t>((i % 13) + ((i / 256) & 3));
    }
    std::vector<uint8_t> comp;
    CHECK(lz4BlockCompress(src.data(), src.size(), comp));
    CHECK(comp.size() < src.size() / 4);

    std::vector<uint8_t> decomp(src.size(), 0);
    std::string err;
    CHECK(lz4BlockDecompress(comp.data(), comp.size(), decomp.data(), decomp.size(), &err));
    CHECK(decomp == src);

    // Reject truncated/corrupt LZ4
    CHECK(!lz4BlockDecompress(comp.data(), comp.size() / 2, decomp.data(), decomp.size(), &err));

    // 2. Snappy literal + copy round-trip hand-constructed block:
    // "abcdefabcdef" -> len=12, tag(lit6)=(5<<2)=20, "abcdef", tag(1-byte copy len=6 -> ((2)<<2)|1 = 9), off=6
    const uint8_t snappyBlock[] = {12, 20, 'a', 'b', 'c', 'd', 'e', 'f', 9, 6};
    std::vector<uint8_t> snapOut;
    CHECK(snappyDecompress(snappyBlock, sizeof(snappyBlock), snapOut, &err));
    CHECK(std::string(snapOut.begin(), snapOut.end()) == "abcdefabcdef");

    const uint8_t badSnappy[] = {10, 0xFC, 0xFF};
    CHECK(!snappyDecompress(badSnappy, sizeof(badSnappy), snapOut, &err));
    return 0;
}

int testCsvIngest() {
    const std::string csv =
        "id,tag,weight,flag,note\r\n"
        "10,alpha,3.5,true,\"hello, world\"\r\n"
        "20,beta,NA,false,\"line1\nline2\"\r\n"
        "30,alpha,7.25,true,\"escaped \"\"q\"\"\"\r\n"
        "40,,0.0,false,\r\n";

    std::string err;
    auto t = ingestCsvFromMemory(csv.data(), csv.size(), CsvIngestOptions{}, &err);
    CHECK(t != nullptr);
    CHECK(t->rowCount() == 4);
    CHECK(t->columnCount() == 5);
    CHECK(t->schema().columns[0].type == ColType::I32);
    CHECK(t->schema().columns[2].type == ColType::F64);
    CHECK(t->schema().columns[3].type == ColType::Bool);

    CHECK(t->getCell(RowLoc{0, 0}, 0).i64 == 10);
    CHECK(t->getCell(RowLoc{0, 0}, 4).str == "hello, world");
    CHECK(t->getCell(RowLoc{0, 1}, 2).isNull);
    CHECK(t->getCell(RowLoc{0, 1}, 4).str == "line1\nline2");
    CHECK(t->getCell(RowLoc{0, 2}, 4).str == "escaped \"q\"");
    CHECK(t->getCell(RowLoc{0, 3}, 1).isNull);
    CHECK(t->getCell(RowLoc{0, 3}, 4).isNull);

    // Streaming file CSV > 12k rows (multi-chunk)
    const auto tmpPath = (std::filesystem::temp_directory_path() / "bro_test_m2_stream.csv").string();
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        out << "idx,val,bucket\n";
        for (int i = 0; i < 12000; ++i) {
            out << i << "," << (i * 0.5) << ",b" << (i % 7) << "\n";
        }
    }
    CsvIngestOptions fopts;
    fopts.explicitSchema = {{"idx", ColType::I32}, {"val", ColType::F64}, {"bucket", ColType::Dict}};
    auto tFile = ingestCsvFromFile(tmpPath, fopts, &err);
    std::error_code ec;
    std::filesystem::remove(tmpPath, ec);
    CHECK(tFile != nullptr);
    CHECK(tFile->rowCount() == 12000);
    CHECK(tFile->chunkCount() == 3);
    CHECK(tFile->getCell(RowLoc{2, 100}, 0).i64 == 8192 + 100);
    return 0;
}

int testParquetRoundTrip() {
    auto tbl = std::make_shared<Table>(std::vector<ColumnDef>{
        {"b", ColType::Bool},
        {"i32c", ColType::I32},
        {"i64c", ColType::I64},
        {"f32c", ColType::F32},
        {"f64c", ColType::F64},
        {"dictc", ColType::Dict},
        {"strc", ColType::String},
    });
    for (int i = 0; i < 500; ++i) {
        std::vector<ScalarValue> row = {
            ScalarValue::makeBool((i % 2) == 0),
            ScalarValue::makeI32(i * 3),
            (i % 9 == 0) ? ScalarValue::makeNull() : ScalarValue::makeI64(int64_t{1000000000LL} + i),
            ScalarValue::makeF32(static_cast<float>(i) * 0.25f),
            ScalarValue::makeF64(static_cast<double>(i) * 0.125),
            ScalarValue::makeDict(tbl->internString(i % 3 == 0 ? "red" : (i % 3 == 1 ? "green" : "blue"))),
            (i % 11 == 0) ? ScalarValue::makeNull() : ScalarValue::makeString("s_" + std::to_string(i)),
        };
        tbl->appendRow(row);
    }

    ParquetSaveOptions sopts;
    sopts.compressLz4 = true;
    sopts.rowGroupRows = 200;
    std::string err;
    std::vector<uint8_t> pq = serializeSnapshotToParquetBytes(*tbl->snapshot(), sopts, &err);
    CHECK(!pq.empty());

    ParquetIngestOptions iopts;
    iopts.stringAsDict = false;
    auto loaded = ingestParquetFromMemory(pq.data(), pq.size(), iopts, &err);
    CHECK(loaded != nullptr);
    CHECK(loaded->rowCount() == 500);
    CHECK(loaded->getCell(RowLoc{0, 18}, 2).isNull);
    CHECK(loaded->getCell(RowLoc{0, 22}, 6).isNull);
    CHECK(loaded->getCell(RowLoc{0, 17}, 1).i64 == 51);
    CHECK(loaded->getCell(RowLoc{0, 17}, 2).i64 == 1000000017LL);
    CHECK(std::abs(loaded->getCell(RowLoc{0, 17}, 3).f64 - 4.25) < 1e-5);
    CHECK(std::abs(loaded->getCell(RowLoc{0, 17}, 4).f64 - 2.125) < 1e-9);
    CHECK(loaded->getCell(RowLoc{0, 17}, 5).str == "blue");
    CHECK(loaded->getCell(RowLoc{0, 17}, 6).str == "s_17");

    // Column projection & file I/O
    const auto tmpPq = (std::filesystem::temp_directory_path() / "bro_test_m2.parquet").string();
    CHECK(saveSnapshotToParquetFile(*tbl->snapshot(), tmpPq, sopts, &err));
    ParquetIngestOptions projOpts;
    projOpts.columns = {"i32c", "strc"};
    projOpts.maxRows = 150;
    auto proj = ingestParquetFromFile(tmpPq, projOpts, &err);
    std::error_code ec;
    std::filesystem::remove(tmpPq, ec);
    CHECK(proj != nullptr);
    CHECK(proj->rowCount() == 150);
    CHECK(proj->columnCount() == 2);
    CHECK(proj->getCell(RowLoc{0, 149}, 0).i64 == 447);
    return 0;
}

int testReactiveAggAndJoin() {
    auto left = std::make_shared<Table>(std::vector<ColumnDef>{
        {"id", ColType::I32},
        {"grp", ColType::Dict},
        {"rkey", ColType::I32},
        {"val", ColType::F64},
    });
    for (int i = 0; i < 16384; ++i) {
        std::vector<ScalarValue> r = {
            ScalarValue::makeI32(i),
            ScalarValue::makeDict(left->internString("k" + std::to_string(i % 5))),
            ScalarValue::makeI32(i % 4),
            ScalarValue::makeF64(10.0 + (i % 7)),
        };
        left->appendRow(r);
    }

    auto right = std::make_shared<Table>(std::vector<ColumnDef>{
        {"id", ColType::I32},
        {"tag", ColType::String},
    });
    right->appendRow(std::vector<ScalarValue>{ScalarValue::makeI32(0), ScalarValue::makeString("zero")});
    right->appendRow(std::vector<ScalarValue>{ScalarValue::makeI32(1), ScalarValue::makeString("one")});
    right->appendRow(std::vector<ScalarValue>{ScalarValue::makeI32(2), ScalarValue::makeString("two")});

    // 1. Grouped aggregation reactive query
    QueryPlan aggPlan;
    const uint32_t flt = aggPlan.exprs.bin(ExprOp::Ge, aggPlan.exprs.col("val"),
                                           aggPlan.exprs.lit(ScalarValue::makeF64(10.0)));
    aggPlan.filters.push_back(flt);
    aggPlan.groupKeys.push_back(aggPlan.exprs.col("grp"));
    aggPlan.groupAliases.push_back("grp");
    aggPlan.aggregates.push_back(AggSpec{AggSpec::Kind::Count, "cnt", kInvalidExpr, ColType::I64});
    aggPlan.aggregates.push_back(AggSpec{AggSpec::Kind::Sum, "tot", aggPlan.exprs.col("val"), ColType::F64});
    aggPlan.aggregates.push_back(AggSpec{AggSpec::Kind::Avg, "mean", aggPlan.exprs.col("val"), ColType::F64});
    aggPlan.aggregates.push_back(AggSpec{AggSpec::Kind::Min, "lo", aggPlan.exprs.col("val"), ColType::F64});
    aggPlan.aggregates.push_back(AggSpec{AggSpec::Kind::Max, "hi", aggPlan.exprs.col("val"), ColType::F64});
    aggPlan.orderBy.push_back(OrderByItem{aggPlan.exprs.col("grp"), false});

    ReactiveQuery rqAgg(left, aggPlan);
    auto initAgg = rqAgg.refresh();
    CHECK(initAgg.changed && initAgg.usedIncrementalAgg);

    // Mutate single row in chunk 2
    CHECK(left->updateCell(RowLoc{2, 50}, 3, ScalarValue::makeF64(777.0)));
    auto incAgg = rqAgg.refresh();
    CHECK(incAgg.changed && incAgg.usedIncrementalAgg);
    CHECK(incAgg.recomputedChunks == 1 && incAgg.reusedChunks == 3);

    auto refAgg = executeQuery(*left->snapshot(), aggPlan);
    CHECK(refAgg.ok && incAgg.table->rowCount() == refAgg.table->rowCount());
    for (uint32_t r = 0; r < incAgg.table->rowCount(); ++r) {
        for (size_t c = 1; c < 6; ++c) {
            CHECK(std::abs(incAgg.table->getCell(RowLoc{0, r}, c).f64 -
                           refAgg.table->getCell(RowLoc{0, r}, c).f64) < 1e-6);
        }
    }

    // 2. Left join reactive query
    QueryPlan joinPlan;
    joinPlan.join.kind = JoinSpec::Kind::Left;
    joinPlan.join.rightTable = right->snapshot();
    joinPlan.join.leftKeys = {"rkey"};
    joinPlan.join.rightKeys = {"id"};
    joinPlan.selectItems.push_back(SelectItem{"lid", joinPlan.exprs.col("id", 0), ColType::I32});
    joinPlan.selectItems.push_back(SelectItem{"rtag", joinPlan.exprs.col("tag", 1), ColType::String});
    joinPlan.selectItems.push_back(SelectItem{"val", joinPlan.exprs.col("val", 0), ColType::F64});

    ReactiveQuery rqJoin(left, joinPlan);
    auto initJoin = rqJoin.refresh();
    CHECK(initJoin.changed && initJoin.usedIncrementalJoin);
    CHECK(initJoin.table->rowCount() == 16384);

    CHECK(left->updateCell(RowLoc{1, 10}, 3, ScalarValue::makeF64(888.0)));
    auto incJoin = rqJoin.refresh();
    CHECK(incJoin.changed && incJoin.usedIncrementalJoin);
    CHECK(incJoin.recomputedChunks == 1 && incJoin.reusedChunks == 3);
    CHECK(incJoin.table->getCell(RowLoc{1, 10}, 2).f64 == 888.0);
    CHECK(incJoin.table->getCell(RowLoc{0, 3}, 1).isNull);
    return 0;
}

int testCompressedAndBundles() {
    auto t1 = std::make_shared<Table>(std::vector<ColumnDef>{{"k", ColType::I32}, {"s", ColType::String}});
    auto t2 = std::make_shared<Table>(std::vector<ColumnDef>{{"v", ColType::F64}});
    for (int i = 0; i < 4000; ++i) {
        t1->appendRow(std::vector<ScalarValue>{ScalarValue::makeI32(i % 16), ScalarValue::makeString("common_string_" + std::to_string(i % 4))});
        t2->appendRow(std::vector<ScalarValue>{ScalarValue::makeF64(i * 1.25)});
    }
    t1->createIndex(0);

    std::vector<uint8_t> raw = serializeSnapshotBytes(*t1->snapshot(), SaveOptions{false});
    std::vector<uint8_t> cmp = serializeSnapshotBytes(*t1->snapshot(), SaveOptions{true});
    CHECK(cmp.size() < raw.size());

    std::string err;
    auto fromV4 = deserializeTableBytes(cmp.data(), cmp.size(), &err);
    CHECK(fromV4 != nullptr && fromV4->rowCount() == 4000 && fromV4->hasIndex(0));

    const auto cmpFile = (std::filesystem::temp_directory_path() / "bro_test_v4.brotable").string();
    const auto bndFile = (std::filesystem::temp_directory_path() / "bro_test_bundle.brotable").string();

    CHECK(saveSnapshotToFile(*t1->snapshot(), cmpFile, SaveOptions{true}, &err));
    auto loadedV4 = loadTableFromFile(cmpFile, &err);
    CHECK(loadedV4 != nullptr && loadedV4->rowCount() == 4000);
    auto mmapV4 = mmapSnapshotFromFile(cmpFile, &err);
    CHECK(mmapV4 == nullptr);

    std::vector<BundleEntryInput> entries = {
        {"entities", t1->snapshot()},
        {"metrics", t2->snapshot()},
    };
    CHECK(saveBundleToFile(entries, bndFile, SaveOptions{false}, &err));

    std::vector<LoadedBundleEntry> loadedBnd;
    CHECK(loadBundleFromFile(bndFile, loadedBnd, &err));
    CHECK(loadedBnd.size() == 2 && loadedBnd[0].table->rowCount() == 4000 &&
          loadedBnd[1].table->rowCount() == 4000);

    std::vector<MappedBundleEntry> mappedBnd;
    CHECK(mmapBundleFromFile(bndFile, mappedBnd, &err, true));
    CHECK(mappedBnd.size() == 2 && mappedBnd[0].snapshot->findIndex(0) != nullptr &&
          mappedBnd[1].snapshot->getCell(0, 0, 10).f64 == 12.5);
    mappedBnd.clear();

    std::error_code ec;
    std::filesystem::remove(cmpFile, ec);
    std::filesystem::remove(bndFile, ec);
    return 0;
}

}  // namespace

int main() {
    if (testCodecs() != 0) return 1;
    if (testCsvIngest() != 0) return 1;
    if (testParquetRoundTrip() != 0) return 1;
    if (testReactiveAggAndJoin() != 0) return 1;
    if (testCompressedAndBundles() != 0) return 1;
    std::cout << "test_table_m2 OK\n";
    return 0;
}
