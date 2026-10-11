#include "util/json_in.h"

#include <cstdlib>

namespace bro::util {

const JsonValue& JsonValue::get(const std::string& key) const {
    static const JsonValue kNull;
    if (type != Type::Object) return kNull;
    const JsonValue* v = find(key);
    return v ? *v : kNull;
}

const JsonValue* JsonValue::find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

namespace {

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string err;

    std::string at(const char* what) const {
        size_t line = 1, col = 1;
        for (size_t k = 0; k < i && k < s.size(); ++k) {
            if (s[k] == '\n') { ++line; col = 1; } else ++col;
        }
        return std::string(what) + " at line " + std::to_string(line) + ", column " + std::to_string(col);
    }

    bool fail(const char* what) {
        if (err.empty()) err = at(what);
        return false;
    }

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    bool literal(const char* word) {
        size_t n = 0;
        while (word[n]) ++n;
        if (s.substr(i, n) != std::string_view(word, n)) return fail("bad literal");
        i += n;
        return true;
    }

    static void putUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return fail("short \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool str(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) break;
            const char e = s[i++];
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
                    unsigned cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        i += 2;
                        unsigned lo;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else { putUtf8(out, 0xFFFD); cp = lo; }
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }

    bool value(JsonValue& v, int depth) {
        if (depth > 256) return fail("nesting too deep");
        ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];
        if (c == '{') {
            v.type = JsonValue::Type::Object;
            ++i; ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!str(key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                ++i;
                JsonValue member;
                if (!value(member, depth + 1)) return false;
                v.object[std::move(key)] = std::move(member);
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = JsonValue::Type::Array;
            ++i; ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;) {
                JsonValue item;
                if (!value(item, depth + 1)) return false;
                v.array.push_back(std::move(item));
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = JsonValue::Type::String; return str(v.string); }
        if (c == 't') { v.type = JsonValue::Type::Bool; v.boolean = true; return literal("true"); }
        if (c == 'f') { v.type = JsonValue::Type::Bool; v.boolean = false; return literal("false"); }
        if (c == 'n') { v.type = JsonValue::Type::Null; return literal("null"); }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const size_t start = i;
            if (s[i] == '-') ++i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                                    s[i] == 'E' || s[i] == '+' || s[i] == '-')) ++i;
            const std::string num(s.substr(start, i - start));
            char* end = nullptr;
            v.type = JsonValue::Type::Number;
            v.number = std::strtod(num.c_str(), &end);
            if (!end || *end != '\0' || num == "-") return fail("bad number");
            return true;
        }
        return fail("unexpected character");
    }
};

}  // namespace

bool parseJson(std::string_view text, JsonValue& out, std::string* error) {
    Parser p{text};
    out = JsonValue{};
    if (!p.value(out, 0)) {
        if (error) *error = p.err;
        return false;
    }
    p.ws();
    if (p.i != text.size()) {
        if (error) *error = p.at("trailing characters");
        return false;
    }
    return true;
}

} // namespace bro::util
