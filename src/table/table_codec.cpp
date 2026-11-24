#include "table/table_codec.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace bro::table {

namespace {

constexpr uint32_t kLz4HashLog = 12;
constexpr size_t kLz4HashSize = 1u << kLz4HashLog;
constexpr size_t kLz4MinMatch = 4;
constexpr size_t kLz4LastLiterals = 5;
constexpr size_t kLz4MfLimit = 12;
constexpr size_t kLz4MaxDist = 65535;
constexpr uint64_t kMaxSnappyUncompressed = 512ULL * 1024ULL * 1024ULL;

inline uint32_t readU32LE(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

inline uint32_t lz4Hash(uint32_t seq) {
    return (seq * 2654435761u) >> (32u - kLz4HashLog);
}

void writeLz4Length(std::vector<uint8_t>& out, size_t len) {
    while (len >= 255) {
        out.push_back(255);
        len -= 255;
    }
    out.push_back(static_cast<uint8_t>(len));
}

bool emitLz4LastLiterals(std::vector<uint8_t>& out, const uint8_t* anchor, size_t litLen) {
    const uint8_t tokenLit = static_cast<uint8_t>(std::min<size_t>(litLen, 15));
    out.push_back(static_cast<uint8_t>(tokenLit << 4));
    if (litLen >= 15) writeLz4Length(out, litLen - 15);
    if (litLen > 0) out.insert(out.end(), anchor, anchor + litLen);
    return true;
}

}  // namespace

bool lz4BlockCompress(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out) {
    out.clear();
    if (srcLen == 0) return true;
    if (!src) return false;
    out.reserve(srcLen + (srcLen / 255) + 16);

    if (srcLen < kLz4MfLimit) {
        return emitLz4LastLiterals(out, src, srcLen);
    }

    std::array<uint32_t, kLz4HashSize> table{};
    table.fill(0);

    const uint8_t* ip = src;
    const uint8_t* anchor = src;
    const uint8_t* const iend = src + srcLen;
    const uint8_t* const mflimit = iend - kLz4MfLimit;
    const uint8_t* const matchLimit = iend - kLz4LastLiterals;

    ++ip;
    while (ip <= mflimit) {
        uint32_t forwardStep = 1;
        uint32_t searchMatchNb = 1u << 6;
        const uint8_t* ref = nullptr;

        while (true) {
            const uint8_t* nextIp = ip + forwardStep;
            forwardStep = searchMatchNb++ >> 6;
            if (nextIp > mflimit) {
                return emitLz4LastLiterals(out, anchor, static_cast<size_t>(iend - anchor));
            }
            const uint32_t h = lz4Hash(readU32LE(ip));
            const size_t curOff = static_cast<size_t>(ip - src);
            const uint32_t refOff = table[h];
            table[h] = static_cast<uint32_t>(curOff);
            if (curOff > refOff && (curOff - refOff) <= kLz4MaxDist &&
                readU32LE(src + refOff) == readU32LE(ip)) {
                ref = src + refOff;
                break;
            }
            ip = nextIp;
        }

        // Extend match backwards into unconsumed literals if possible.
        while (ip > anchor && ref > src && ip[-1] == ref[-1]) {
            --ip;
            --ref;
        }

        const size_t litLen = static_cast<size_t>(ip - anchor);
        const size_t offset = static_cast<size_t>(ip - ref);
        const size_t tokenIdx = out.size();
        out.push_back(0);
        if (litLen >= 15) writeLz4Length(out, litLen - 15);
        if (litLen > 0) out.insert(out.end(), anchor, anchor + litLen);

        out.push_back(static_cast<uint8_t>(offset & 0xFFu));
        out.push_back(static_cast<uint8_t>((offset >> 8) & 0xFFu));

        ip += kLz4MinMatch;
        ref += kLz4MinMatch;
        const uint8_t* const matchStart = ip;
        while (ip < matchLimit && *ip == *ref) {
            ++ip;
            ++ref;
        }
        const size_t matchLenExtra = static_cast<size_t>(ip - matchStart);
        const size_t tokenLit = std::min<size_t>(litLen, 15);
        const size_t tokenMatch = std::min<size_t>(matchLenExtra, 15);
        out[tokenIdx] = static_cast<uint8_t>((tokenLit << 4) | tokenMatch);
        if (matchLenExtra >= 15) writeLz4Length(out, matchLenExtra - 15);

        anchor = ip;
        if (ip > mflimit) break;
        const size_t prevOff = static_cast<size_t>(ip - src) - 2;
        table[lz4Hash(readU32LE(src + prevOff))] = static_cast<uint32_t>(prevOff);
    }

    return emitLz4LastLiterals(out, anchor, static_cast<size_t>(iend - anchor));
}

bool lz4BlockDecompress(const uint8_t* src, size_t srcLen, uint8_t* dst,
                        size_t expectedDstLen, std::string* err) {
    if (expectedDstLen == 0) return srcLen == 0;
    if (!src || !dst || srcLen == 0) {
        if (err) *err = "lz4BlockDecompress: empty input or null buffer";
        return false;
    }

    size_t sp = 0;
    size_t dp = 0;
    while (sp < srcLen) {
        const uint8_t token = src[sp++];
        size_t litLen = token >> 4;
        if (litLen == 15) {
            uint8_t s = 0;
            do {
                if (sp >= srcLen) {
                    if (err) *err = "lz4BlockDecompress: truncated literal length";
                    return false;
                }
                s = src[sp++];
                if (litLen > expectedDstLen) {
                    if (err) *err = "lz4BlockDecompress: literal length overflow";
                    return false;
                }
                litLen += s;
            } while (s == 255);
        }

        if (litLen > srcLen - sp || litLen > expectedDstLen - dp) {
            if (err) *err = "lz4BlockDecompress: literal bounds exceeded";
            return false;
        }
        if (litLen > 0) {
            std::memcpy(dst + dp, src + sp, litLen);
            sp += litLen;
            dp += litLen;
        }

        if (sp == srcLen) break;

        if (srcLen - sp < 2) {
            if (err) *err = "lz4BlockDecompress: truncated match offset";
            return false;
        }
        const size_t offset = static_cast<size_t>(src[sp]) | (static_cast<size_t>(src[sp + 1]) << 8);
        sp += 2;
        if (offset == 0 || offset > dp) {
            if (err) *err = "lz4BlockDecompress: invalid match offset";
            return false;
        }

        size_t matchLen = token & 0x0Fu;
        if (matchLen == 15) {
            uint8_t s = 0;
            do {
                if (sp >= srcLen) {
                    if (err) *err = "lz4BlockDecompress: truncated match length";
                    return false;
                }
                s = src[sp++];
                if (matchLen > expectedDstLen) {
                    if (err) *err = "lz4BlockDecompress: match length overflow";
                    return false;
                }
                matchLen += s;
            } while (s == 255);
        }
        matchLen += kLz4MinMatch;
        if (matchLen > expectedDstLen - dp) {
            if (err) *err = "lz4BlockDecompress: match exceeds expected output size";
            return false;
        }

        if (offset >= matchLen) {
            std::memcpy(dst + dp, dst + dp - offset, matchLen);
            dp += matchLen;
        } else {
            for (size_t i = 0; i < matchLen; ++i) {
                dst[dp] = dst[dp - offset];
                ++dp;
            }
        }
    }

    if (dp != expectedDstLen) {
        if (err) *err = "lz4BlockDecompress: output size mismatch";
        return false;
    }
    return true;
}

bool snappyDecompress(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out,
                      std::string* err) {
    out.clear();
    if (!src || srcLen == 0) {
        if (err) *err = "snappyDecompress: empty input";
        return false;
    }

    size_t sp = 0;
    uint64_t expectedLen = 0;
    uint32_t shift = 0;
    while (true) {
        if (sp >= srcLen || shift > 28) {
            if (err) *err = "snappyDecompress: invalid uncompressed length varint";
            return false;
        }
        const uint8_t b = src[sp++];
        expectedLen |= static_cast<uint64_t>(b & 0x7Fu) << shift;
        if ((b & 0x80u) == 0) break;
        shift += 7;
    }
    if (expectedLen > kMaxSnappyUncompressed) {
        if (err) *err = "snappyDecompress: uncompressed length exceeds safety cap";
        return false;
    }

    out.resize(static_cast<size_t>(expectedLen));
    size_t dp = 0;
    const size_t cap = out.size();

    while (sp < srcLen) {
        const uint8_t tag = src[sp++];
        const uint8_t type = tag & 0x03u;
        if (type == 0) {
            size_t len = static_cast<size_t>(tag >> 2);
            if (len < 60) {
                len += 1;
            } else {
                const size_t nbytes = len - 59;
                if (sp + nbytes > srcLen) {
                    if (err) *err = "snappyDecompress: truncated literal length";
                    return false;
                }
                len = 0;
                for (size_t i = 0; i < nbytes; ++i) {
                    len |= static_cast<size_t>(src[sp++]) << (8 * i);
                }
                len += 1;
            }
            if (len > srcLen - sp || len > cap - dp) {
                if (err) *err = "snappyDecompress: literal bounds exceeded";
                return false;
            }
            if (len > 0) std::memcpy(out.data() + dp, src + sp, len);
            sp += len;
            dp += len;
        } else {
            size_t len = 0;
            size_t offset = 0;
            if (type == 1) {
                if (sp >= srcLen) {
                    if (err) *err = "snappyDecompress: truncated 1-byte copy";
                    return false;
                }
                len = 4u + ((tag >> 2) & 0x07u);
                offset = (static_cast<size_t>(tag & 0xE0u) << 3) | static_cast<size_t>(src[sp++]);
            } else if (type == 2) {
                if (sp + 2 > srcLen) {
                    if (err) *err = "snappyDecompress: truncated 2-byte copy";
                    return false;
                }
                len = 1u + (tag >> 2);
                offset = static_cast<size_t>(src[sp]) | (static_cast<size_t>(src[sp + 1]) << 8);
                sp += 2;
            } else {
                if (sp + 4 > srcLen) {
                    if (err) *err = "snappyDecompress: truncated 4-byte copy";
                    return false;
                }
                len = 1u + (tag >> 2);
                offset = static_cast<size_t>(src[sp]) |
                         (static_cast<size_t>(src[sp + 1]) << 8) |
                         (static_cast<size_t>(src[sp + 2]) << 16) |
                         (static_cast<size_t>(src[sp + 3]) << 24);
                sp += 4;
            }
            if (offset == 0 || offset > dp || len > cap - dp) {
                if (err) *err = "snappyDecompress: copy out of bounds";
                return false;
            }
            if (offset >= len) {
                std::memcpy(out.data() + dp, out.data() + dp - offset, len);
                dp += len;
            } else {
                for (size_t i = 0; i < len; ++i) {
                    out[dp] = out[dp - offset];
                    ++dp;
                }
            }
        }
    }

    if (dp != cap) {
        if (err) *err = "snappyDecompress: output size mismatch";
        return false;
    }
    return true;
}

}  // namespace bro::table
