#pragma once

// A small streaming JSON writer: what a status reply or a trace dump builds
// its text with, without a JSON library. Commas and nesting are tracked, so
// a caller writes keys and values in order and gets well-formed output:
//
//   JsonOut j;
//   j.beginObject();
//   j.key("fps").number(59.9);
//   j.key("frames").beginArray();
//   for (auto& f : frames) j.number(f.ms);
//   j.endArray();
//   j.endObject();
//   std::string text = j.str();
//
// Non-finite numbers are written as null (JSON has no NaN).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace bro::util {

class JsonOut {
public:
    JsonOut& beginObject() { value(); out_ += '{'; stack_.push_back(true); return *this; }
    JsonOut& endObject() { out_ += '}'; stack_.pop_back(); return *this; }
    JsonOut& beginArray() { value(); out_ += '['; stack_.push_back(true); return *this; }
    JsonOut& endArray() { out_ += ']'; stack_.pop_back(); return *this; }

    JsonOut& key(std::string_view k) {
        comma();
        quote(k);
        out_ += ':';
        afterKey_ = true;
        return *this;
    }

    JsonOut& string(std::string_view s) { value(); quote(s); return *this; }
    JsonOut& boolean(bool b) { value(); out_ += b ? "true" : "false"; return *this; }
    JsonOut& null() { value(); out_ += "null"; return *this; }
    JsonOut& integer(int64_t v) { value(); out_ += std::to_string(v); return *this; }
    JsonOut& number(double v, int decimals = 3) {
        value();
        if (!std::isfinite(v)) { out_ += "null"; return *this; }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        // Trim trailing zeros (and a bare point) so 16.000 reads 16.
        std::string s(buf);
        if (s.find('.') != std::string::npos) {
            while (!s.empty() && s.back() == '0') s.pop_back();
            if (!s.empty() && s.back() == '.') s.pop_back();
        }
        if (s == "-0") s = "0";
        out_ += s;
        return *this;
    }
    /// Text that is already JSON (a nested reply), inserted as one value.
    JsonOut& raw(std::string_view json) { value(); out_ += json; return *this; }

    const std::string& str() const { return out_; }
    std::string take() { return std::move(out_); }

    static std::string quoted(std::string_view s) {
        JsonOut j;
        j.quote(s);
        return j.out_;
    }

private:
    void comma() {
        if (stack_.empty()) return;
        if (stack_.back()) stack_.back() = false;
        else out_ += ',';
    }
    void value() {
        if (afterKey_) { afterKey_ = false; return; }
        comma();
    }
    void quote(std::string_view s) {
        out_ += '"';
        for (unsigned char c : s) {
            switch (c) {
                case '"': out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out_ += buf;
                    } else {
                        out_ += static_cast<char>(c);
                    }
            }
        }
        out_ += '"';
    }

    std::string out_;
    std::vector<bool> stack_;  // per open container: no element written yet
    bool afterKey_ = false;
};

}  // namespace bro::util
