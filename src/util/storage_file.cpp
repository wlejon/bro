#include "util/storage_file.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace bro::util {

namespace {

// ---------------------------------------------------------------------------
// A JSON reader that knows exactly as much as a Storage file needs: one object
// of string values. Anything else that is valid JSON is skipped over so a
// stray number or nested object costs that key, not the file.
// ---------------------------------------------------------------------------
class Reader {
public:
    explicit Reader(const std::string& s) : s_(s) {}

    bool readObject(std::map<std::string, std::string>& out) {
        ws();
        if (!accept('{')) return false;
        ws();
        if (accept('}')) return true;
        for (;;) {
            ws();
            std::string key;
            if (!readString(key)) return false;
            ws();
            if (!accept(':')) return false;
            ws();
            if (peek() == '"') {
                std::string value;
                if (!readString(value)) return false;
                out[key] = std::move(value);
            } else if (!skipValue()) {
                return false;
            }
            ws();
            if (accept(',')) continue;
            return accept('}');
        }
    }

private:
    const std::string& s_;
    size_t p_ = 0;

    char peek() const { return p_ < s_.size() ? s_[p_] : '\0'; }
    bool accept(char c) { if (peek() == c) { p_++; return true; } return false; }
    void ws() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) p_++;
    }

    static int hexVal(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool readHex4(unsigned& cp) {
        if (p_ + 4 > s_.size()) return false;
        cp = 0;
        for (int i = 0; i < 4; i++) {
            int v = hexVal(s_[p_ + i]);
            if (v < 0) return false;
            cp = (cp << 4) | static_cast<unsigned>(v);
        }
        p_ += 4;
        return true;
    }

    static void putUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool readString(std::string& out) {
        if (!accept('"')) return false;
        while (p_ < s_.size()) {
            char c = s_[p_++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p_ >= s_.size()) return false;
            char e = s_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!readHex4(cp)) return false;
                    // A high surrogate followed by `\uDCxx` is one code point.
                    if (cp >= 0xD800 && cp <= 0xDBFF && p_ + 6 <= s_.size() &&
                        s_[p_] == '\\' && s_[p_ + 1] == 'u') {
                        size_t save = p_;
                        p_ += 2;
                        unsigned lo = 0;
                        if (readHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        } else {
                            p_ = save;
                        }
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    // Step over one value of any kind. Strings are walked with their escapes
    // so a brace inside one does not count; everything else is bracket depth.
    bool skipValue() {
        if (peek() == '"') { std::string sink; return readString(sink); }
        int depth = 0;
        while (p_ < s_.size()) {
            char c = s_[p_];
            if (c == '"') { std::string sink; if (!readString(sink)) return false; continue; }
            if (c == '{' || c == '[') depth++;
            else if (c == '}' || c == ']') {
                if (depth == 0) return true;   // the enclosing object's close
                depth--;
            } else if (c == ',' && depth == 0) return true;
            p_++;
        }
        return depth == 0;
    }
};

} // namespace

std::string jsonQuote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);   // UTF-8 passes through
                }
        }
    }
    out += '"';
    return out;
}

bool readStorageFile(const std::string& path, std::map<std::string, std::string>& out) {
    out.clear();
    if (path.empty()) return false;
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    Reader r(content);
    // A file that stops short still yields every complete pair before the
    // break; the return value says whether the whole document parsed.
    return r.readObject(out);
}

bool writeStorageFile(const std::string& path, const std::map<std::string, std::string>& items) {
    if (path.empty()) return false;
    namespace fs = std::filesystem;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        file << "{\n";
        bool first = true;
        for (const auto& [k, v] : items) {
            if (!first) file << ",\n";
            file << "  " << jsonQuote(k) << ": " << jsonQuote(v);
            first = false;
        }
        file << "\n}\n";
        file.flush();
        if (!file.good()) {
            file.close();
            std::error_code ec;
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);   // replaces an existing file on every platform
    if (ec) {
        // A reader holding the old file open can defeat the replace on
        // Windows; step out of its way rather than leaving the new document
        // stranded under the .tmp name.
        std::error_code ec2;
        fs::remove(path, ec2);
        fs::rename(tmp, path, ec2);
        if (ec2) { fs::remove(tmp, ec2); return false; }
    }
    return true;
}

} // namespace bro::util
