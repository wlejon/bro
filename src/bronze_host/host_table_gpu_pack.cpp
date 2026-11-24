#include "bronze_host/host_table.h"

#include "embed/embed.h"
#include "table/table.h"

#if BRO_WITH_KERNEL
#include "bronze_host/host_kernel_gpu.h"
#include <brokernel/api.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

using namespace bro::table;

#if BRO_WITH_KERNEL

class HostPackKernelBuffer final : public brokernel::api::GpuBuffer {
public:
    explicit HostPackKernelBuffer(size_t byteLen) : data_(byteLen, 0) {}

    uint64_t address() const override { return 0; }
    size_t bytes() const override { return data_.size(); }
    size_t capacity() const override { return data_.size(); }
    brokernel::api::BufferResidency residency() const override {
        return brokernel::api::BufferResidency::Host;
    }
    void* hostDataForCpu() override { return data_.data(); }
    bool prepareForCpu(std::string*) override { return true; }

    bool write(size_t offset, const void* src, size_t n, std::string* err) override {
        if (offset > data_.size() || n > (data_.size() - offset)) {
            if (err) *err = "HostPackKernelBuffer write out of bounds";
            return false;
        }
        if (n > 0) std::memcpy(data_.data() + offset, src, n);
        return true;
    }

    bool read(size_t offset, void* out, size_t n, std::string* err) override {
        if (offset > data_.size() || n > (data_.size() - offset)) {
            if (err) *err = "HostPackKernelBuffer read out of bounds";
            return false;
        }
        if (n > 0) std::memcpy(out, data_.data() + offset, n);
        return true;
    }

    std::vector<uint8_t>& mutableBytes() { return data_; }

private:
    std::vector<uint8_t> data_;
};

enum class PackLayout { AoS, SoA };
enum class PackDtype { F32, F64, I32 };

size_t packDtypeSize(PackDtype dt) {
    switch (dt) {
        case PackDtype::F32: return sizeof(float);
        case PackDtype::F64: return sizeof(double);
        case PackDtype::I32: return sizeof(int32_t);
    }
    return 4;
}

bool parsePackDtype(const std::string& s, PackDtype& out) {
    if (s == "f32" || s == "fp32" || s == "float32") { out = PackDtype::F32; return true; }
    if (s == "f64" || s == "fp64" || s == "float64") { out = PackDtype::F64; return true; }
    if (s == "i32" || s == "int32") { out = PackDtype::I32; return true; }
    return false;
}

bool parsePackLayout(const std::string& s, PackLayout& out) {
    if (s == "aos" || s == "AoS" || s == "row") { out = PackLayout::AoS; return true; }
    if (s == "soa" || s == "SoA" || s == "col") { out = PackLayout::SoA; return true; }
    return false;
}

Value argAt(std::span<const Value> args, size_t idx) {
    return idx < args.size() ? args[idx] : ev::undefined();
}

bool parseColumnList(const Schema& schema, Value colsArg, std::vector<size_t>& out, const char* fnName) {
    if (isJsArray(colsArg)) {
        ev::Persistent arr(colsArg);
        const uint32_t n = static_cast<uint32_t>(std::max(0.0, ev::toDouble(ev::getProperty(arr.get(), "length"))));
        out.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            int idx = resolveTableColumnArg(schema, ev::getElement(arr.get(), i));
            if (idx < 0) {
                ev::throwRangeError(std::string(fnName) + ": invalid column in list");
                return false;
            }
            out.push_back(static_cast<size_t>(idx));
        }
    } else if (!colsArg.isUndefined() && !colsArg.isNull()) {
        int idx = resolveTableColumnArg(schema, colsArg);
        if (idx < 0) {
            ev::throwRangeError(std::string(fnName) + ": invalid column");
            return false;
        }
        out.push_back(static_cast<size_t>(idx));
    } else {
        ev::throwTypeError(std::string(fnName) + ": expected column name/index or array of columns");
        return false;
    }
    if (out.empty()) {
        ev::throwTypeError(std::string(fnName) + ": empty column list");
        return false;
    }
    for (size_t c : out) {
        if (schema.columns[c].type == ColType::String) {
            ev::throwTypeError(std::string(fnName) + ": string column '" + schema.columns[c].name + "' is unsupported");
            return false;
        }
    }
    return true;
}

template <typename DstT>
void convertColumnSlice(const ColumnChunk& col, ColType ct, uint32_t rowBegin, uint32_t rowCount,
                        const TableSnapshot::FrozenChunk& fc, size_t colOffsetBytes,
                        size_t elemStrideBytes, uint8_t* dstBase) {
    const uint8_t* raw = col.rawData();
    const bool dense = fc.allAlive();
    size_t outIdx = 0;
    auto writeOne = [&](uint32_t r) {
        DstT val{};
        switch (ct) {
            case ColType::F32: val = static_cast<DstT>(reinterpret_cast<const float*>(raw)[r]); break;
            case ColType::F64: val = static_cast<DstT>(reinterpret_cast<const double*>(raw)[r]); break;
            case ColType::I32:
            case ColType::Dict: val = static_cast<DstT>(reinterpret_cast<const int32_t*>(raw)[r]); break;
            case ColType::U32: val = static_cast<DstT>(reinterpret_cast<const uint32_t*>(raw)[r]); break;
            case ColType::I64: val = static_cast<DstT>(reinterpret_cast<const int64_t*>(raw)[r]); break;
            case ColType::Bool: val = static_cast<DstT>(raw[r] != 0 ? 1 : 0); break;
            case ColType::String: break;
        }
        std::memcpy(dstBase + outIdx * elemStrideBytes + colOffsetBytes, &val, sizeof(DstT));
        ++outIdx;
    };
    if (dense) {
        for (uint32_t r = 0; r < rowCount; ++r) writeOne(rowBegin + r);
    } else {
        for (uint32_t r = 0; r < rowCount; ++r) {
            if (fc.isAlive(rowBegin + r)) writeOne(rowBegin + r);
        }
    }
}

void writePackedCell(ColumnChunk& col, ColType ct, uint32_t row, PackDtype dt, const uint8_t* src) {
    double fval = 0.0;
    int64_t ival = 0;
    bool isFloat = false;
    if (dt == PackDtype::F32) {
        float v = 0.0f;
        std::memcpy(&v, src, sizeof(float));
        fval = static_cast<double>(v);
        ival = static_cast<int64_t>(v);
        isFloat = true;
    } else if (dt == PackDtype::F64) {
        std::memcpy(&fval, src, sizeof(double));
        ival = static_cast<int64_t>(fval);
        isFloat = true;
    } else {
        int32_t v = 0;
        std::memcpy(&v, src, sizeof(int32_t));
        ival = static_cast<int64_t>(v);
        fval = static_cast<double>(v);
    }
    switch (ct) {
        case ColType::F32: {
            float out = static_cast<float>(fval);
            std::memcpy(col.rawData() + static_cast<size_t>(row) * sizeof(float), &out, sizeof(float));
            break;
        }
        case ColType::F64: {
            std::memcpy(col.rawData() + static_cast<size_t>(row) * sizeof(double), &fval, sizeof(double));
            break;
        }
        case ColType::I32: {
            int32_t out = static_cast<int32_t>(isFloat ? static_cast<int64_t>(fval) : ival);
            std::memcpy(col.rawData() + static_cast<size_t>(row) * sizeof(int32_t), &out, sizeof(int32_t));
            break;
        }
        case ColType::U32: {
            uint32_t out = static_cast<uint32_t>(isFloat ? static_cast<int64_t>(fval) : ival);
            std::memcpy(col.rawData() + static_cast<size_t>(row) * sizeof(uint32_t), &out, sizeof(uint32_t));
            break;
        }
        case ColType::I64: {
            int64_t out = isFloat ? static_cast<int64_t>(fval) : ival;
            std::memcpy(col.rawData() + static_cast<size_t>(row) * sizeof(int64_t), &out, sizeof(int64_t));
            break;
        }
        case ColType::Bool: {
            col.rawData()[row] = (isFloat ? (fval != 0.0) : (ival != 0)) ? 1 : 0;
            break;
        }
        case ColType::Dict: {
            uint32_t out = static_cast<uint32_t>(isFloat ? static_cast<int64_t>(fval) : ival);
            std::memcpy(col.rawData() + static_cast<size_t>(row) * sizeof(uint32_t), &out, sizeof(uint32_t));
            break;
        }
        case ColType::String:
            break;
    }
}

std::string defaultGpuResidency() {
    return engineCompute() ? "gpu" : "host";
}

Value allocateOrWriteTarget(const std::shared_ptr<brokernel::api::GpuBuffer>& target,
                            Value targetVal,
                            const std::string& residency,
                            const std::vector<uint8_t>& packed,
                            const char* fnName) {
    const size_t totalBytes = packed.size();
    std::string err;
    if (target) {
        if (target->bytes() < totalBytes) {
            return ev::throwRangeError(std::string(fnName) + ": target KernelBuffer (" +
                                       std::to_string(target->bytes()) + " B) is smaller than required (" +
                                       std::to_string(totalBytes) + " B)");
        }
        if (totalBytes > 0 && !target->write(0, packed.data(), totalBytes, &err)) {
            return ev::throwError(std::string(fnName) + ": " + (err.empty() ? "target write failed" : err));
        }
        return targetVal;
    }

    if (residency == "gpu" || residency == "shared") {
        auto kb = createEngineBuffer(totalBytes, &err);
        if (!kb) return ev::throwError(std::string(fnName) + ": " + err);
        if (totalBytes > 0 && !kb->write(0, packed.data(), totalBytes, &err)) {
            return ev::throwError(std::string(fnName) + ": " + err);
        }
        return brokernel::api::bufferValue(std::move(kb));
    }

    auto hostKb = std::make_shared<HostPackKernelBuffer>(totalBytes);
    if (totalBytes > 0) std::memcpy(hostKb->mutableBytes().data(), packed.data(), totalBytes);
    return brokernel::api::bufferValue(std::move(hostKb));
}

Value packTableColumnsToKernelBuffer(const TableSnapshot& snap, Value colsArg, Value optsVal) {
    std::vector<size_t> cols;
    if (!parseColumnList(snap.schema, colsArg, cols, "packToKernelBuffer")) {
        return ev::undefined();
    }

    PackLayout layout = PackLayout::AoS;
    PackDtype dt = PackDtype::F32;
    size_t strideElems = cols.size();
    std::string residency = defaultGpuResidency();
    std::shared_ptr<brokernel::api::GpuBuffer> target;
    ev::Persistent targetHold(ev::undefined());

    if (ev::isObject(optsVal)) {
        ev::Persistent opts(optsVal);
        Value lVal = ev::getProperty(opts.get(), "layout");
        if (ev::isString(lVal) && !parsePackLayout(ev::toUtf8(lVal), layout)) {
            return ev::throwTypeError("packToKernelBuffer: layout must be 'aos' or 'soa'");
        }
        Value dVal = ev::getProperty(opts.get(), "dtype");
        if (ev::isString(dVal) && !parsePackDtype(ev::toUtf8(dVal), dt)) {
            return ev::throwTypeError("packToKernelBuffer: dtype must be 'f32', 'f64', or 'i32'");
        }
        Value sVal = ev::getProperty(opts.get(), "strideFloats");
        if (sVal.isUndefined()) sVal = ev::getProperty(opts.get(), "stride");
        if (sVal.isNumber()) {
            const double sv = sVal.asNumber();
            if (sv < static_cast<double>(cols.size())) {
                return ev::throwRangeError("packToKernelBuffer: strideFloats must be >= column count");
            }
            strideElems = static_cast<size_t>(sv);
        }
        Value rVal = ev::getProperty(opts.get(), "residency");
        if (ev::isString(rVal)) residency = ev::toUtf8(rVal);
        Value tVal = ev::getProperty(opts.get(), "target");
        if (!tVal.isUndefined() && !tVal.isNull()) {
            target = brokernel::api::bufferOf(tVal);
            if (!target) return ev::throwTypeError("packToKernelBuffer: opts.target is not a KernelBuffer");
            targetHold.set(tVal);
        }
    }

    const size_t elemBytes = packDtypeSize(dt);
    const size_t rowCount = snap.rowCount();
    const size_t totalBytes = (layout == PackLayout::AoS)
                                  ? rowCount * strideElems * elemBytes
                                  : cols.size() * rowCount * elemBytes;

    std::vector<uint8_t> packed(totalBytes, 0);
    if (rowCount > 0) {
        size_t chunkRowOffset = 0;
        for (const auto& fc : snap.chunks) {
            if (fc.aliveCount == 0) continue;
            for (size_t ci = 0; ci < cols.size(); ++ci) {
                const size_t c = cols[ci];
                const ColType ct = snap.schema.columns[c].type;
                const ColumnChunk& colChunk = *fc.columns[c];
                size_t colOffBytes = 0;
                size_t stepBytes = 0;
                uint8_t* dstPtr = packed.data();
                if (layout == PackLayout::AoS) {
                    colOffBytes = ci * elemBytes;
                    stepBytes = strideElems * elemBytes;
                    dstPtr += chunkRowOffset * stepBytes;
                } else {
                    colOffBytes = 0;
                    stepBytes = elemBytes;
                    dstPtr += (ci * rowCount + chunkRowOffset) * elemBytes;
                }
                if (dt == PackDtype::F32) {
                    convertColumnSlice<float>(colChunk, ct, 0, fc.rowCount, fc, colOffBytes, stepBytes, dstPtr);
                } else if (dt == PackDtype::F64) {
                    convertColumnSlice<double>(colChunk, ct, 0, fc.rowCount, fc, colOffBytes, stepBytes, dstPtr);
                } else {
                    convertColumnSlice<int32_t>(colChunk, ct, 0, fc.rowCount, fc, colOffBytes, stepBytes, dstPtr);
                }
            }
            chunkRowOffset += fc.aliveCount;
        }
    }

    return allocateOrWriteTarget(target, targetHold.get(), residency, packed, "packToKernelBuffer");
}

Value unpackTableFromKernelBuffer(Table& tbl, Value colsArg, Value kbVal, Value optsVal) {
    auto kb = brokernel::api::bufferOf(kbVal);
    if (!kb) return ev::throwTypeError("Table.unpackFromKernelBuffer: expects (columns, kernelBuffer, opts?)");

    std::vector<size_t> cols;
    if (!parseColumnList(tbl.schema(), colsArg, cols, "Table.unpackFromKernelBuffer")) {
        return ev::undefined();
    }

    PackLayout layout = PackLayout::AoS;
    PackDtype dt = PackDtype::F32;
    size_t strideElems = cols.size();
    size_t rowOffset = 0;
    size_t bufferRowOffset = 0;
    size_t maxRows = SIZE_MAX;
    size_t totalRowsSoa = tbl.rowCount();
    bool hasSoaTotal = false;

    if (ev::isObject(optsVal)) {
        ev::Persistent opts(optsVal);
        Value lVal = ev::getProperty(opts.get(), "layout");
        if (ev::isString(lVal) && !parsePackLayout(ev::toUtf8(lVal), layout)) {
            return ev::throwTypeError("Table.unpackFromKernelBuffer: layout must be 'aos' or 'soa'");
        }
        Value dVal = ev::getProperty(opts.get(), "dtype");
        if (ev::isString(dVal) && !parsePackDtype(ev::toUtf8(dVal), dt)) {
            return ev::throwTypeError("Table.unpackFromKernelBuffer: dtype must be 'f32', 'f64', or 'i32'");
        }
        Value sVal = ev::getProperty(opts.get(), "strideFloats");
        if (sVal.isUndefined()) sVal = ev::getProperty(opts.get(), "stride");
        if (sVal.isNumber()) {
            const double sv = sVal.asNumber();
            if (sv < static_cast<double>(cols.size())) {
                return ev::throwRangeError("Table.unpackFromKernelBuffer: strideFloats must be >= column count");
            }
            strideElems = static_cast<size_t>(sv);
        }
        Value oVal = ev::getProperty(opts.get(), "rowOffset");
        if (oVal.isNumber()) rowOffset = static_cast<size_t>(std::max(0.0, oVal.asNumber()));
        Value boVal = ev::getProperty(opts.get(), "bufferRowOffset");
        if (boVal.isNumber()) bufferRowOffset = static_cast<size_t>(std::max(0.0, boVal.asNumber()));
        Value cVal = ev::getProperty(opts.get(), "rowCount");
        if (cVal.isNumber()) maxRows = static_cast<size_t>(std::max(0.0, cVal.asNumber()));
        Value tVal = ev::getProperty(opts.get(), "totalRows");
        if (tVal.isNumber()) {
            totalRowsSoa = static_cast<size_t>(std::max(0.0, tVal.asNumber()));
            hasSoaTotal = true;
        }
    }

    const size_t elemBytes = packDtypeSize(dt);
    const size_t kbBytes = kb->bytes();
    if (kbBytes == 0 || tbl.rowCount() <= rowOffset || maxRows == 0) {
        return ev::fromDouble(0);
    }

    size_t availRows = 0;
    if (layout == PackLayout::AoS) {
        availRows = kbBytes / (strideElems * elemBytes);
    } else {
        if (!hasSoaTotal && totalRowsSoa == 0) totalRowsSoa = kbBytes / (cols.size() * elemBytes);
        const size_t colStrideBytes = totalRowsSoa * elemBytes;
        availRows = (colStrideBytes > 0 && kbBytes >= cols.size() * colStrideBytes)
                        ? totalRowsSoa
                        : (kbBytes / (cols.size() * elemBytes));
        totalRowsSoa = availRows;
    }
    if (availRows <= bufferRowOffset) return ev::fromDouble(0);
    const size_t remainingTableRows = tbl.rowCount() - rowOffset;
    const size_t remainingBufRows = availRows - bufferRowOffset;
    const size_t copyRows = std::min({remainingTableRows, maxRows, remainingBufRows});
    if (copyRows == 0) return ev::fromDouble(0);

    std::vector<uint8_t> hostBytes(kbBytes);
    std::string err;
    if (!kb->read(0, hostBytes.data(), kbBytes, &err)) {
        return ev::throwError("Table.unpackFromKernelBuffer: " + err);
    }

    const size_t endRow = rowOffset + copyRows;
    size_t chunkStartAlive = 0;
    for (size_t ci = 0; ci < tbl.chunkCount() && chunkStartAlive < endRow; ++ci) {
        const uint32_t rc = tbl.chunk(ci).rowCount;
        const uint32_t ac = tbl.chunk(ci).aliveCount;
        if (rc == 0 || ac == 0) continue;
        const size_t chunkEndAlive = chunkStartAlive + ac;
        if (chunkEndAlive <= rowOffset) {
            chunkStartAlive = chunkEndAlive;
            continue;
        }

        // Only COW-detach and mark dirty chunks whose alive rows are actually touched.
        tbl.ensureChunkUnique(ci);
        TableChunk& ch = tbl.chunkMut(ci);
        size_t aliveCursor = chunkStartAlive;
        for (uint32_t r = 0; r < rc && aliveCursor < endRow; ++r) {
            if (!ch.isAlive(r)) continue;
            if (aliveCursor >= rowOffset) {
                const size_t srcIdx = bufferRowOffset + (aliveCursor - rowOffset);
                for (size_t k = 0; k < cols.size(); ++k) {
                    const size_t colIdx = cols[k];
                    const ColType ct = tbl.schema().columns[colIdx].type;
                    const size_t byteOff = (layout == PackLayout::AoS)
                                               ? (srcIdx * strideElems + k) * elemBytes
                                               : (k * totalRowsSoa + srcIdx) * elemBytes;
                    if (byteOff + elemBytes <= hostBytes.size()) {
                        writePackedCell(*ch.columns[colIdx], ct, r, dt, hostBytes.data() + byteOff);
                    }
                }
            }
            ++aliveCursor;
        }
        tbl.markChunkDirty(ci);
        chunkStartAlive = chunkEndAlive;
    }

    for (size_t colIdx : cols) tbl.notifyColumnUpdated(colIdx);
    return ev::fromDouble(static_cast<double>(copyRows));
}

#endif  // BRO_WITH_KERNEL

}  // namespace

void attachTableGpuPackProto(ev::ObjectBuilder& proto) {
#if BRO_WITH_KERNEL
    proto.def("packToKernelBuffer", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.packToKernelBuffer: invalid receiver");
        return packTableColumnsToKernelBuffer(*tbl->snapshot(), argAt(args, 0), argAt(args, 1));
    });

    proto.def("unpackFromKernelBuffer", 3, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.unpackFromKernelBuffer: invalid receiver");
        return unpackTableFromKernelBuffer(*tbl, argAt(args, 0), argAt(args, 1), argAt(args, 2));
    });
#else
    (void)proto;
#endif
}

void attachSnapshotGpuPackProto(ev::ObjectBuilder& proto) {
#if BRO_WITH_KERNEL
    proto.def("packToKernelBuffer", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.packToKernelBuffer: invalid receiver");
        return packTableColumnsToKernelBuffer(*snap, argAt(args, 0), argAt(args, 1));
    });
#else
    (void)proto;
#endif
}

}  // namespace bro::bronze_host
