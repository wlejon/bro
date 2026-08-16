#include "util/string_utils.h"

#include <algorithm>
#include <cctype>

namespace bro::util {

std::string trim(const std::string& str) {
    auto start = str.find_first_not_of(" \t\n\r\f\v");
    if (start == std::string::npos) return "";
    auto end = str.find_last_not_of(" \t\n\r\f\v");
    return str.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    for (char ch : str) {
        if (ch == delimiter) {
            tokens.push_back(token);
            token.clear();
        } else {
            token += ch;
        }
    }
    tokens.push_back(token);
    return tokens;
}

std::string toLower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

std::string toUpper(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

bool startsWith(const std::string& str, const std::string& prefix) {
    if (prefix.size() > str.size()) return false;
    return str.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& str, const std::string& suffix) {
    if (suffix.size() > str.size()) return false;
    return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string replace(const std::string& str, const std::string& from, const std::string& to) {
    if (from.empty()) return str;
    std::string result = str;
    size_t pos = 0;
    while ((pos = result.find(from, pos)) != std::string::npos) {
        result.replace(pos, from.size(), to);
        pos += to.size();
    }
    return result;
}

std::vector<uint8_t> base64Decode(const std::string& s) {
    static int8_t table[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[(unsigned char)alpha[i]] = (int8_t)i;
        init = true;
    }
    std::vector<uint8_t> out;
    out.reserve(s.size() * 3 / 4);
    uint32_t buf = 0;
    int bits = 0;
    for (unsigned char c : s) {
        if (c == '=' || c <= ' ') continue;
        int v = table[c];
        if (v < 0) continue;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back((uint8_t)((buf >> bits) & 0xff));
        }
    }
    return out;
}

std::string base64Encode(const uint8_t* data, size_t len) {
    static const char alpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    if (!data || len == 0) return out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(alpha[(v >> 18) & 0x3f]);
        out.push_back(alpha[(v >> 12) & 0x3f]);
        out.push_back(alpha[(v >> 6) & 0x3f]);
        out.push_back(alpha[v & 0x3f]);
    }
    // Tail: 1 or 2 bytes, zero-extended to a full group and padded with '='.
    if (i < len) {
        const size_t rem = len - i;
        uint32_t v = uint32_t(data[i]) << 16;
        if (rem == 2) v |= uint32_t(data[i + 1]) << 8;
        out.push_back(alpha[(v >> 18) & 0x3f]);
        out.push_back(alpha[(v >> 12) & 0x3f]);
        out.push_back(rem == 2 ? alpha[(v >> 6) & 0x3f] : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace bro::util