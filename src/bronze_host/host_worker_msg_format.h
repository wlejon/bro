#pragma once

// The structured-clone byte format shared by the writer (host_worker_msg.cpp)
// and the reader (host_worker_msg_read.cpp). Private to those two files.
//
// A value is a tag and a body. Objects are numbered in the order written
// (HTML's serialization memory), and kObjectRef names an earlier one. Two
// per-message tables keep repeated structure from being re-sent:
//
//   key ref     u32 id. id == keys-so-far defines the next key, followed by
//               u8 utf16 + u32 length + the code units; a smaller id reuses
//               one. kLiteralKey is a one-off key with the same body.
//   layout      kObjectLayout u32 id. id == layouts-so-far defines one:
//               u32 count + that many key refs. Then one value per key.
//               Ten thousand objects of one shape send their keys once and
//               the reader builds them along one cached shape transition.
//
// Strings are sent as their code units (Latin-1 or UTF-16), not transcoded.
// The reader bounds-checks everything: bro.net feeds it bytes off the wire.

#include <cstdint>
#include <cstring>

namespace bro::bronze_host::clonefmt {

enum Tag : uint8_t {
    kUndefined       = 0x00,
    kNull            = 0x01,
    kTrue            = 0x02,
    kFalse           = 0x03,
    kInt32           = 0x04,
    kFloat64         = 0x05,
    kString          = 0x06,  // UTF-8 (reader only; the writer sends code units)
    kArray           = 0x07,
    kObject          = 0x08,  // u32 count, then (key ref, value) pairs
    kArrayBuffer     = 0x09,  // inline bytes
    kBufferBlock     = 0x0A,  // u32 index into Message::buffers
    kTypedArray      = 0x0B,
    kBigInt          = 0x0D,
    kTransferImageBitmap = 0x0E,
    kDate            = 0x0F,
    kRegExp          = 0x10,
    kMap             = 0x11,
    kSet             = 0x12,
    kError           = 0x13,
    kDataView        = 0x14,
    kTransferMesh    = 0x15,
    kObjectRef       = 0x16,
    kObjectLayout    = 0x17,
    kStringLatin1    = 0x18,  // u32 length + bytes
    kStringUtf16     = 0x19,  // u32 length + 2*length bytes
};

inline constexpr uint32_t kLiteralKey = 0xFFFFFFFFu;

// Buffers at least this large leave `data` for Message::buffers on a Local
// clone: past it the receiver's adopting the block beats copying it twice.
inline constexpr uint32_t kOutOfBandMin = 4096;

inline constexpr int kMaxDepth = 64;

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool ok(size_t n) const { return n <= size_ - pos_; }
    size_t remaining() const { return size_ - pos_; }
    uint8_t u8() { return data_[pos_++]; }
    uint32_t u32() {
        uint32_t v;
        std::memcpy(&v, data_ + pos_, 4);
        pos_ += 4;
        return v;
    }
    double f64() {
        double v;
        std::memcpy(&v, data_ + pos_, 8);
        pos_ += 8;
        return v;
    }
    const uint8_t* ptr(size_t n) {
        const uint8_t* p = data_ + pos_;
        pos_ += n;
        return p;
    }
private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

}  // namespace bro::bronze_host::clonefmt
