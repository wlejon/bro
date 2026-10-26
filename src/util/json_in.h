#pragma once

// bro's JSON reader, the counterpart of json_out.h, for what the engine reads
// itself without a JSON library: bro.json and the grant store before any JS
// realm exists (engine/app_manifest.cpp, engine/grant_store.cpp), a service's
// reply (a Hugging Face
// tree listing, util/model_fetch.h). Parses RFC 8259 JSON into a JsonValue
// tree; numbers are doubles, strings are UTF-8 (\u escapes, surrogate pairs
// included); a repeated key keeps its last value, as JSON.parse does.
// Comments are not JSON and fail like any other syntax error.
//
//   bro::util::JsonValue v;
//   std::string err;
//   if (!bro::util::parseJson(text, v, &err)) ...;
//   for (const auto& e : v.array) e.get("path").string ...

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bro::util {

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;

    bool isObject() const { return type == Type::Object; }
    bool isArray() const { return type == Type::Array; }
    bool isString() const { return type == Type::String; }
    bool isNumber() const { return type == Type::Number; }

    /// The member `key` of an object, or a shared Null value.
    const JsonValue& get(const std::string& key) const;
    /// The member `key` of an object, or null when it has none (an explicit
    /// `null` member is found).
    const JsonValue* find(const std::string& key) const;
};

/// Parse `text` (the whole of it, whitespace around allowed) into `out`.
/// False with a message ("expected ':' at line 3, column 9") in `*error` on
/// malformed input or nesting deeper than 256.
bool parseJson(std::string_view text, JsonValue& out, std::string* error = nullptr);

} // namespace bro::util
