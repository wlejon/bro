#include "bronze_host/host_table.h"

#include "bronze_host/host_builder.h"
#include "bronze_host/host_values.h"
#include "embed/embed_host_class.h"
#include "table/table.h"

#if BRO_WITH_KERNEL
#include "bronze_host/host_kernel_gpu.h"
#include <brokernel/api.h>
#endif

#if BRO_WITH_TENSOR
#include <brotensor/api.h>
#include <brotensor/kernel_interop.h>
#include <brotensor/runtime.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace bro::bronze_host {

namespace {

using namespace bro::table;

Value interopArgAt(std::span<const Value> args, size_t i) {
    return hasArg(args, i) ? args[i] : ev::undefined();
}

#if BRO_WITH_KERNEL

class TableChunkKernelBuffer final : public brokernel::api::GpuBuffer {
public:
    TableChunkKernelBuffer(std::shared_ptr<const ColumnChunk> col, size_t byteLen, bool writable)
        : col_(std::move(col)), byteLen_(byteLen), writable_(writable) {}

    uint64_t address() const override { return 0; }
    size_t bytes() const override { return byteLen_; }
    size_t capacity() const override { return col_ ? col_->fixedByteCapacity() : 0; }
    brokernel::api::BufferResidency residency() const override {
        return brokernel::api::BufferResidency::Host;
    }
    void* hostDataForCpu() override {
        return col_ ? const_cast<uint8_t*>(col_->rawData()) : nullptr;
    }
    bool prepareForCpu(std::string*) override { return col_ != nullptr; }

    bool write(size_t offset, const void* data, size_t n, std::string* err) override {
        if (!writable_) {
            if (err) *err = "read-only snapshot KernelBuffer";
            return false;
        }
        if (!col_ || offset > byteLen_ || n > (byteLen_ - offset)) {
            if (err) *err = "TableChunkKernelBuffer write out of bounds";
            return false;
        }
        if (n > 0) {
            std::memcpy(const_cast<uint8_t*>(col_->rawData()) + offset, data, n);
        }
        return true;
    }

    bool read(size_t offset, void* out, size_t n, std::string* err) override {
        if (!col_ || offset > byteLen_ || n > (byteLen_ - offset)) {
            if (err) *err = "TableChunkKernelBuffer read out of bounds";
            return false;
        }
        if (n > 0) {
            std::memcpy(out, col_->rawData() + offset, n);
        }
        return true;
    }

private:
    std::shared_ptr<const ColumnChunk> col_;
    size_t byteLen_ = 0;
    bool writable_ = false;
};

class HostOwnedKernelBuffer final : public brokernel::api::GpuBuffer {
public:
    explicit HostOwnedKernelBuffer(size_t byteLen) : data_(byteLen, 0) {}

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
            if (err) *err = "HostOwnedKernelBuffer write out of bounds";
            return false;
        }
        if (n > 0) std::memcpy(data_.data() + offset, src, n);
        return true;
    }

    bool read(size_t offset, void* out, size_t n, std::string* err) override {
        if (offset > data_.size() || n > (data_.size() - offset)) {
            if (err) *err = "HostOwnedKernelBuffer read out of bounds";
            return false;
        }
        if (n > 0) std::memcpy(out, data_.data() + offset, n);
        return true;
    }

    std::vector<uint8_t>& mutableBytes() { return data_; }

private:
    std::vector<uint8_t> data_;
};

Value packColumnToKernelBuffer(const TableSnapshot& snap, size_t colIdx, Value optsVal) {
    const ColType ct = snap.schema.columns[colIdx].type;
    if (ct == ColType::String) {
        return ev::throwTypeError("toKernelBuffer: string columns are variable-length");
    }
    const size_t elemBytes = colTypeElemSize(ct);
    const size_t totalBytes = snap.rowCount() * elemBytes;

    std::string residency = "host";
    if (ev::isObject(optsVal)) {
        Value r = ev::getProperty(optsVal, "residency");
        if (ev::isString(r)) residency = ev::toUtf8(r);
    }

    std::shared_ptr<brokernel::api::GpuBuffer> kb;
    std::string err;
    if (residency == "shared" || residency == "gpu") {
        kb = createEngineBuffer(totalBytes, &err);
        if (!kb) return ev::throwError("toKernelBuffer: " + err);
        std::vector<uint8_t> packed(totalBytes);
        size_t dstOff = 0;
        for (const auto& fc : snap.chunks) {
            const uint8_t* raw = fc.columns[colIdx]->rawData();
            if (fc.allAlive()) {
                const size_t nbytes = static_cast<size_t>(fc.rowCount) * elemBytes;
                if (nbytes > 0) std::memcpy(packed.data() + dstOff, raw, nbytes);
                dstOff += nbytes;
            } else {
                for (uint32_t r = 0; r < fc.rowCount; ++r) {
                    if (fc.isAlive(r)) {
                        std::memcpy(packed.data() + dstOff, raw + static_cast<size_t>(r) * elemBytes, elemBytes);
                        dstOff += elemBytes;
                    }
                }
            }
        }
        if (totalBytes > 0 && !kb->write(0, packed.data(), totalBytes, &err)) {
            return ev::throwError("toKernelBuffer: " + err);
        }
        return brokernel::api::bufferValue(std::move(kb));
    }

    auto owned = std::make_shared<HostOwnedKernelBuffer>(totalBytes);
    uint8_t* dst = owned->mutableBytes().data();
    size_t dstOff = 0;
    for (const auto& fc : snap.chunks) {
        const uint8_t* raw = fc.columns[colIdx]->rawData();
        if (fc.allAlive()) {
            const size_t nbytes = static_cast<size_t>(fc.rowCount) * elemBytes;
            if (nbytes > 0) std::memcpy(dst + dstOff, raw, nbytes);
            dstOff += nbytes;
        } else {
            for (uint32_t r = 0; r < fc.rowCount; ++r) {
                if (fc.isAlive(r)) {
                    std::memcpy(dst + dstOff, raw + static_cast<size_t>(r) * elemBytes, elemBytes);
                    dstOff += elemBytes;
                }
            }
        }
    }
    return brokernel::api::bufferValue(std::move(owned));
}

Value readColumnFromKernelBuffer(Table& tbl, std::span<const Value> args) {
    bool chunkForm = false;
    size_t chunkIdx = 0;
    int colIdx = -1;
    std::shared_ptr<brokernel::api::GpuBuffer> kb;

    if (hasArg(args, 1) && (kb = brokernel::api::bufferOf(args[1])) != nullptr) {
        colIdx = resolveTableColumnArg(tbl.schema(), args[0]);
    } else if (hasArg(args, 2) && (kb = brokernel::api::bufferOf(args[2])) != nullptr) {
        chunkForm = true;
        chunkIdx = args[0].isNumber() ? static_cast<size_t>(std::max(0.0, args[0].asNumber())) : SIZE_MAX;
        colIdx = resolveTableColumnArg(tbl.schema(), args[1]);
    } else {
        return ev::throwTypeError("Table.readFromKernelBuffer: expects (col, kb) or (chunkIdx, col, kb)");
    }

    if (colIdx < 0 || static_cast<size_t>(colIdx) >= tbl.columnCount()) {
        return ev::throwRangeError("Table.readFromKernelBuffer: invalid column");
    }
    const size_t c = static_cast<size_t>(colIdx);
    const ColType ct = tbl.schema().columns[c].type;
    if (ct == ColType::String) {
        return ev::throwTypeError("Table.readFromKernelBuffer: string columns are variable-length");
    }
    const size_t elemBytes = colTypeElemSize(ct);
    const size_t totalKbBytes = kb->bytes();
    std::vector<uint8_t> hostBytes(totalKbBytes);
    std::string err;
    if (totalKbBytes > 0 && !kb->read(0, hostBytes.data(), totalKbBytes, &err)) {
        return ev::throwError("Table.readFromKernelBuffer: " + err);
    }

    if (chunkForm) {
        if (chunkIdx >= tbl.chunkCount()) {
            return ev::throwRangeError("Table.readFromKernelBuffer: chunk index out of bounds");
        }
        tbl.ensureChunkUnique(chunkIdx);
        TableChunk& ch = tbl.chunkMut(chunkIdx);
        const size_t availElems = totalKbBytes / elemBytes;
        const size_t copyCount = std::min<size_t>(availElems, ch.rowCount);
        if (copyCount > 0) {
            std::memcpy(ch.columns[c]->rawData(), hostBytes.data(), copyCount * elemBytes);
        }
        tbl.markChunkDirty(chunkIdx);
        tbl.notifyColumnUpdated(c);
        return ev::fromDouble(static_cast<double>(copyCount));
    }

    const size_t availElems = totalKbBytes / elemBytes;
    size_t srcIdx = 0;
    for (size_t ci = 0; ci < tbl.chunkCount() && srcIdx < availElems; ++ci) {
        const uint32_t rc = tbl.chunk(ci).rowCount;
        const uint32_t ac = tbl.chunk(ci).aliveCount;
        if (rc == 0 || ac == 0) continue;
        tbl.ensureChunkUnique(ci);
        TableChunk& ch = tbl.chunkMut(ci);
        uint8_t* dst = ch.columns[c]->rawData();
        if (ch.allAlive()) {
            const size_t n = std::min<size_t>(rc, availElems - srcIdx);
            std::memcpy(dst, hostBytes.data() + srcIdx * elemBytes, n * elemBytes);
            srcIdx += n;
        } else {
            for (uint32_t r = 0; r < rc && srcIdx < availElems; ++r) {
                if (ch.isAlive(r)) {
                    std::memcpy(dst + static_cast<size_t>(r) * elemBytes,
                                hostBytes.data() + srcIdx * elemBytes, elemBytes);
                    ++srcIdx;
                }
            }
        }
        tbl.markChunkDirty(ci);
    }
    tbl.notifyColumnUpdated(c);
    return ev::fromDouble(static_cast<double>(srcIdx));
}

#endif  // BRO_WITH_KERNEL

#if BRO_WITH_TENSOR

bool parseTensorDtypeString(const std::string& s, brotensor::Dtype& out) {
    if (s == "f32" || s == "fp32" || s == "float32") { out = brotensor::Dtype::FP32; return true; }
    if (s == "f64" || s == "fp64" || s == "float64") { out = brotensor::Dtype::F64; return true; }
    if (s == "i32" || s == "int32") { out = brotensor::Dtype::INT32; return true; }
    if (s == "i8" || s == "int8" || s == "bool") { out = brotensor::Dtype::INT8; return true; }
    return false;
}

bool parseTensorDeviceString(const std::string& s, brotensor::Device& out) {
    if (s == "cpu") { out = brotensor::Device::CPU; return true; }
    if (s == "vulkan" || s == "vk") { out = brotensor::Device::vulkan(0); return true; }
    if (s == "cuda") { out = brotensor::Device::cuda(0); return true; }
    if (s == "metal") { out = brotensor::Device::metal(0); return true; }
    return false;
}

double scalarToDoubleVal(const ScalarValue& sv) {
    if (colTypeIsFloat(sv.type)) return sv.f64;
    if (colTypeIsUnsignedInt(sv.type)) return static_cast<double>(sv.u64);
    return static_cast<double>(sv.i64);
}

int64_t scalarToI64Val(const ScalarValue& sv) {
    if (colTypeIsFloat(sv.type)) return static_cast<int64_t>(sv.f64);
    if (colTypeIsUnsignedInt(sv.type)) return static_cast<int64_t>(sv.u64);
    return sv.i64;
}

Value exportSnapshotToTensor(const TableSnapshot& snap, Value colsArg, Value optsArg) {
    std::vector<size_t> colIndices;
    if (isJsArray(colsArg)) {
        ev::Persistent arr(colsArg);
        const uint32_t n = static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr.get(), "length")));
        colIndices.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            int idx = resolveTableColumnArg(snap.schema, ev::getElement(arr.get(), i));
            if (idx < 0) return ev::throwRangeError("toTensor: invalid column in array");
            colIndices.push_back(static_cast<size_t>(idx));
        }
    } else if (!colsArg.isUndefined() && !colsArg.isNull()) {
        int idx = resolveTableColumnArg(snap.schema, colsArg);
        if (idx < 0) return ev::throwRangeError("toTensor: invalid column");
        colIndices.push_back(static_cast<size_t>(idx));
    } else {
        return ev::throwTypeError("toTensor: expects column name/index or array of columns");
    }

    if (colIndices.empty()) return ev::throwTypeError("toTensor: empty column list");
    for (size_t c : colIndices) {
        if (snap.schema.columns[c].type == ColType::String) {
            return ev::throwTypeError("toTensor: cannot export string column '" +
                                      snap.schema.columns[c].name + "'");
        }
    }

    bool allF64 = true;
    bool allI32 = true;
    bool allBool = true;
    for (size_t c : colIndices) {
        ColType ct = snap.schema.columns[c].type;
        if (ct != ColType::F64) allF64 = false;
        if (ct != ColType::I32) allI32 = false;
        if (ct != ColType::Bool) allBool = false;
    }

    brotensor::Dtype dt = brotensor::Dtype::FP32;
    if (allF64) dt = brotensor::Dtype::F64;
    else if (allI32) dt = brotensor::Dtype::INT32;
    else if (allBool) dt = brotensor::Dtype::INT8;

    brotensor::Device dev = brotensor::default_device();
    if (ev::isObject(optsArg)) {
        ev::Persistent o(optsArg);
        Value dVal = ev::getProperty(o.get(), "dtype");
        if (ev::isString(dVal) && !parseTensorDtypeString(ev::toUtf8(dVal), dt)) {
            return ev::throwTypeError("toTensor: unsupported dtype '" + ev::toUtf8(dVal) + "'");
        }
        Value devVal = ev::getProperty(o.get(), "device");
        if (ev::isString(devVal) && !parseTensorDeviceString(ev::toUtf8(devVal), dev)) {
            return ev::throwTypeError("toTensor: unsupported device '" + ev::toUtf8(devVal) + "'");
        }
    }

    const int rows = static_cast<int>(snap.rowCount());
    const int cols = static_cast<int>(colIndices.size());
    if (rows == 0) {
        return brotensor::api::createGpuTensorValue(brotensor::Tensor::empty_on(dev, 0, cols, dt));
    }

    const size_t numel = static_cast<size_t>(rows) * static_cast<size_t>(cols);
    const size_t elemSize = static_cast<size_t>(brotensor::dtype_size_bytes(dt));
    std::vector<uint8_t> raw(numel * elemSize);

    auto writeElem = [&](size_t flatIdx, const ScalarValue& sv) {
        uint8_t* dst = raw.data() + flatIdx * elemSize;
        if (dt == brotensor::Dtype::FP32) {
            float v = static_cast<float>(scalarToDoubleVal(sv));
            std::memcpy(dst, &v, sizeof(float));
        } else if (dt == brotensor::Dtype::F64) {
            double v = scalarToDoubleVal(sv);
            std::memcpy(dst, &v, sizeof(double));
        } else if (dt == brotensor::Dtype::INT32) {
            int32_t v = static_cast<int32_t>(scalarToI64Val(sv));
            std::memcpy(dst, &v, sizeof(int32_t));
        } else {
            int8_t v = static_cast<int8_t>(scalarToI64Val(sv));
            std::memcpy(dst, &v, sizeof(int8_t));
        }
    };

    size_t rowOut = 0;
    for (uint32_t ci = 0; ci < snap.chunks.size(); ++ci) {
        const auto& fc = snap.chunks[ci];
        for (uint32_t r = 0; r < fc.rowCount; ++r) {
            if (!fc.isAlive(r)) continue;
            for (size_t k = 0; k < colIndices.size(); ++k) {
                ScalarValue sv = fc.columns[colIndices[k]]->readCell(r, nullptr);
                writeElem(rowOut * static_cast<size_t>(cols) + k, sv);
            }
            ++rowOut;
        }
    }

    try {
        brotensor::Tensor t =
            brotensor::Tensor::from_raw_bytes_on(dev, raw.data(), rows, cols, dt, raw.size());
        return brotensor::api::createGpuTensorValue(std::move(t));
    } catch (const std::exception& ex) {
        return ev::throwError(std::string("toTensor: ") + ex.what());
    }
}

#endif  // BRO_WITH_TENSOR

}  // namespace

void attachTableInteropProto(ev::ObjectBuilder& proto) {
#if BRO_WITH_KERNEL
    proto.def("chunkKernelBuffer", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.chunkKernelBuffer: invalid receiver");
        const size_t ci = hasArg(args, 0) ? static_cast<size_t>(std::max(0.0, args[0].asNumber())) : SIZE_MAX;
        if (ci >= tbl->chunkCount()) return ev::throwRangeError("Table.chunkKernelBuffer: chunk out of bounds");
        int col = hasArg(args, 1) ? resolveTableColumnArg(tbl->schema(), args[1]) : -1;
        if (col < 0) return ev::throwRangeError("Table.chunkKernelBuffer: invalid column");
        const ColType ct = tbl->schema().columns[static_cast<size_t>(col)].type;
        if (ct == ColType::String) return ev::throwTypeError("Table.chunkKernelBuffer: string columns unsupported");
        tbl->ensureChunkUnique(ci);
        const TableChunk& ch = tbl->chunk(ci);
        const size_t byteLen = static_cast<size_t>(ch.rowCount) * colTypeElemSize(ct);
        auto kb = std::make_shared<TableChunkKernelBuffer>(ch.columns[static_cast<size_t>(col)], byteLen, true);
        return brokernel::api::bufferValue(std::move(kb));
    });

    proto.def("toKernelBuffer", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.toKernelBuffer: invalid receiver");
        int col = hasArg(args, 0) ? resolveTableColumnArg(tbl->schema(), args[0]) : -1;
        if (col < 0) return ev::throwRangeError("Table.toKernelBuffer: invalid column");
        return packColumnToKernelBuffer(*tbl->snapshot(), static_cast<size_t>(col), interopArgAt(args, 1));
    });

    proto.def("readFromKernelBuffer", 3, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.readFromKernelBuffer: invalid receiver");
        return readColumnFromKernelBuffer(*tbl, args);
    });
#else
    (void)proto;
#endif

#if BRO_WITH_TENSOR
    proto.def("toTensor", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto tbl = hostMutableTableOf(thisVal);
        if (!tbl) return ev::throwTypeError("Table.toTensor: invalid receiver");
        return exportSnapshotToTensor(*tbl->snapshot(), interopArgAt(args, 0), interopArgAt(args, 1));
    });
#endif
}

void attachSnapshotInteropProto(ev::ObjectBuilder& proto) {
#if BRO_WITH_KERNEL
    proto.def("chunkKernelBuffer", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.chunkKernelBuffer: invalid receiver");
        const size_t ci = hasArg(args, 0) ? static_cast<size_t>(std::max(0.0, args[0].asNumber())) : SIZE_MAX;
        if (ci >= snap->chunks.size()) return ev::throwRangeError("TableSnapshot.chunkKernelBuffer: chunk out of bounds");
        int col = hasArg(args, 1) ? resolveTableColumnArg(snap->schema, args[1]) : -1;
        if (col < 0) return ev::throwRangeError("TableSnapshot.chunkKernelBuffer: invalid column");
        const ColType ct = snap->schema.columns[static_cast<size_t>(col)].type;
        if (ct == ColType::String) return ev::throwTypeError("TableSnapshot.chunkKernelBuffer: string columns unsupported");
        const auto& fc = snap->chunks[ci];
        const size_t byteLen = static_cast<size_t>(fc.rowCount) * colTypeElemSize(ct);
        auto kb = std::make_shared<TableChunkKernelBuffer>(fc.columns[static_cast<size_t>(col)], byteLen, false);
        return brokernel::api::bufferValue(std::move(kb));
    });

    proto.def("toKernelBuffer", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.toKernelBuffer: invalid receiver");
        int col = hasArg(args, 0) ? resolveTableColumnArg(snap->schema, args[0]) : -1;
        if (col < 0) return ev::throwRangeError("TableSnapshot.toKernelBuffer: invalid column");
        return packColumnToKernelBuffer(*snap, static_cast<size_t>(col), interopArgAt(args, 1));
    });
#else
    (void)proto;
#endif

#if BRO_WITH_TENSOR
    proto.def("toTensor", 2, [](Value thisVal, std::span<const Value> args) -> Value {
        auto snap = hostTableSnapshotOf(thisVal);
        if (!snap) return ev::throwTypeError("TableSnapshot.toTensor: invalid receiver");
        return exportSnapshotToTensor(*snap, interopArgAt(args, 0), interopArgAt(args, 1));
    });
#endif
}

void populateTableInteropNamespace(ev::ObjectBuilder& ns) {
#if BRO_WITH_TENSOR
    ns.def("fromTensor", 2, [](Value, std::span<const Value> args) -> Value {
        if (!hasArg(args, 0)) {
            return ev::throwTypeError("bro.table.fromTensor: expects (tensor, colNamesOrSchema?)");
        }
        brotensor::Tensor* t = brotensor::api::getTensorFromHandle(args[0]);
        if (!t) return ev::throwTypeError("bro.table.fromTensor: expected GpuTensor");

        const int rows = std::max(0, t->rows);
        const int cols = std::max(1, t->cols);
        ColType defaultCt = ColType::F32;
        if (t->dtype == brotensor::Dtype::FP32) defaultCt = ColType::F32;
        else if (t->dtype == brotensor::Dtype::F64) defaultCt = ColType::F64;
        else if (t->dtype == brotensor::Dtype::INT32) defaultCt = ColType::I32;
        else if (t->dtype == brotensor::Dtype::INT8) defaultCt = ColType::I32;
        else return ev::throwTypeError("bro.table.fromTensor: unsupported tensor dtype");

        std::vector<ColumnDef> defs(static_cast<size_t>(cols));
        for (int c = 0; c < cols; ++c) {
            defs[static_cast<size_t>(c)] = ColumnDef{"c" + std::to_string(c), defaultCt};
        }

        if (hasArg(args, 1) && isJsArray(args[1])) {
            ev::Persistent arr(args[1]);
            const uint32_t n = static_cast<uint32_t>(ev::toDouble(ev::getProperty(arr.get(), "length")));
            for (uint32_t c = 0; c < std::min<uint32_t>(n, static_cast<uint32_t>(cols)); ++c) {
                defs[c].name = ev::toUtf8(ev::getElement(arr.get(), c));
            }
        } else if (hasArg(args, 1) && ev::isObject(args[1])) {
            ev::Persistent obj(args[1]);
            auto gObj = ev::globalValue("Object");
            if (gObj.found) {
                ev::Persistent keysFn(ev::getProperty(gObj.value, "keys"));
                Value arg = obj.get();
                ev::CallResult kr = ev::call(keysFn.get(), ev::undefined(), std::span<const Value>(&arg, 1));
                if (!kr.thrown && ev::isObject(kr.value)) {
                    ev::Persistent keysArr(kr.value);
                    const uint32_t n =
                        static_cast<uint32_t>(ev::toDouble(ev::getProperty(keysArr.get(), "length")));
                    for (uint32_t c = 0; c < std::min<uint32_t>(n, static_cast<uint32_t>(cols)); ++c) {
                        std::string name = ev::toUtf8(ev::getElement(keysArr.get(), c));
                        std::string typeStr = ev::toUtf8(ev::getProperty(obj.get(), name));
                        ColType ct = defaultCt;
                        if (parseColType(typeStr, ct)) defs[c].type = ct;
                        defs[c].name = std::move(name);
                    }
                }
            }
        } else if (hasArg(args, 1) && ev::isString(args[1]) && cols == 1) {
            defs[0].name = ev::toUtf8(args[1]);
        }

        const size_t nbytes = t->bytes();
        std::vector<uint8_t> raw(nbytes);
        std::string err;
        if (nbytes > 0 && !brotensor::interop::copy_bytes_from_tensor(*t, raw.data(), nbytes, &err)) {
            return ev::throwError("bro.table.fromTensor: " + err);
        }

        auto tbl = std::make_shared<Table>(defs);
        if (rows == 0) return wrapHostTable(std::move(tbl));

        if (t->dtype == brotensor::Dtype::INT8) {
            const int8_t* src = reinterpret_cast<const int8_t*>(raw.data());
            std::vector<int32_t> i32Cols;
            std::vector<uint8_t> boolCols;
            std::vector<const void*> ptrs(static_cast<size_t>(cols), nullptr);
            bool anyBool = false;
            for (const auto& d : defs) {
                if (d.type == ColType::Bool) anyBool = true;
            }
            i32Cols.resize(static_cast<size_t>(rows) * static_cast<size_t>(cols));
            if (anyBool) boolCols.resize(static_cast<size_t>(rows) * static_cast<size_t>(cols));
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < cols; ++c) {
                    const int8_t v = src[static_cast<size_t>(r) * cols + c];
                    const size_t off = static_cast<size_t>(c) * rows + r;
                    if (defs[static_cast<size_t>(c)].type == ColType::Bool) boolCols[off] = (v != 0) ? 1 : 0;
                    else i32Cols[off] = static_cast<int32_t>(v);
                }
            }
            for (int c = 0; c < cols; ++c) {
                ptrs[static_cast<size_t>(c)] =
                    (defs[static_cast<size_t>(c)].type == ColType::Bool)
                        ? static_cast<const void*>(boolCols.data() + static_cast<size_t>(c) * rows)
                        : static_cast<const void*>(i32Cols.data() + static_cast<size_t>(c) * rows);
            }
            tbl->appendTypedBatch(static_cast<uint32_t>(rows), ptrs.data());
            return wrapHostTable(std::move(tbl));
        }

        const size_t elemBytes = static_cast<size_t>(brotensor::dtype_size_bytes(t->dtype));
        if (cols == 1) {
            const void* ptrs[1] = {raw.data()};
            tbl->appendTypedBatch(static_cast<uint32_t>(rows), ptrs);
            return wrapHostTable(std::move(tbl));
        }

        std::vector<uint8_t> colMajor(static_cast<size_t>(rows) * static_cast<size_t>(cols) * elemBytes);
        std::vector<const void*> ptrs(static_cast<size_t>(cols), nullptr);
        for (int c = 0; c < cols; ++c) {
            uint8_t* dst = colMajor.data() + static_cast<size_t>(c) * rows * elemBytes;
            ptrs[static_cast<size_t>(c)] = dst;
            for (int r = 0; r < rows; ++r) {
                const uint8_t* s = raw.data() + (static_cast<size_t>(r) * cols + c) * elemBytes;
                std::memcpy(dst + static_cast<size_t>(r) * elemBytes, s, elemBytes);
            }
        }
        tbl->appendTypedBatch(static_cast<uint32_t>(rows), ptrs.data());
        return wrapHostTable(std::move(tbl));
    });
#else
    (void)ns;
#endif
}

}  // namespace bro::bronze_host
