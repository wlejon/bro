#pragma once

// Incremental SHA-1 and SHA-256 (FIPS 180-4), portable and dependency-free:
// what bro.models.fetch checks a downloaded file against while it streams to
// disk (util/model_fetch.h). SHA-256 is a Hugging Face LFS file's oid; SHA-1
// is a small file's git blob id, taken over "blob <size>\0" + the bytes.
//
//   bro::util::Sha256 h;
//   h.update(data, n);            // any number of times
//   std::string hex = h.hex();    // lower-case; the object is spent after this

#include <cstddef>
#include <cstdint>
#include <string>

namespace bro::util {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t n);
    std::string hex();  // finishes the hash

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t fill_ = 0;
    uint64_t bytes_ = 0;
};

class Sha1 {
public:
    Sha1();
    void update(const void* data, size_t n);
    std::string hex();  // finishes the hash

private:
    void block(const uint8_t* p);
    uint32_t h_[5];
    uint8_t buf_[64];
    size_t fill_ = 0;
    uint64_t bytes_ = 0;
};

} // namespace bro::util
