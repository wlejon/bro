#include "util/object_url.h"

#include "util/string_utils.h"

#include <cctype>
#include <mutex>
#include <unordered_map>

namespace bro::util {

namespace {

std::mutex& tableMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<std::string, std::shared_ptr<const ObjectURLData>>& table() {
    static std::unordered_map<std::string, std::shared_ptr<const ObjectURLData>> t;
    return t;
}

} // namespace

bool isObjectURL(const std::string& url) {
    return url.compare(0, 5, "blob:") == 0;
}

void registerObjectURL(const std::string& url, std::vector<uint8_t> bytes,
                       std::string type) {
    auto data = std::make_shared<ObjectURLData>();
    data->bytes = std::move(bytes);
    data->type = std::move(type);
    std::lock_guard<std::mutex> lock(tableMutex());
    table()[url] = std::move(data);
}

void revokeObjectURL(const std::string& url) {
    std::lock_guard<std::mutex> lock(tableMutex());
    table().erase(url);
}

std::shared_ptr<const ObjectURLData> lookupObjectURL(const std::string& url) {
    std::lock_guard<std::mutex> lock(tableMutex());
    auto it = table().find(url);
    return it == table().end() ? nullptr : it->second;
}

namespace {

// Percent-decoding for a non-base64 data: body (`data:image/svg+xml,<svg…>`).
std::string percentDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

} // namespace

bool inlineURLBytes(const std::string& url, std::vector<uint8_t>& out,
                    std::string* mime) {
    if (isObjectURL(url)) {
        auto data = lookupObjectURL(url);
        if (!data) return false;
        out = data->bytes;
        if (mime) *mime = data->type;
        return true;
    }

    if (url.compare(0, 5, "data:") != 0) return false;
    const auto comma = url.find(',');
    if (comma == std::string::npos) return false;

    const std::string meta = url.substr(5, comma - 5);
    const std::string body = url.substr(comma + 1);
    if (mime) {
        const auto semi = meta.find(';');
        *mime = meta.substr(0, semi);
    }
    if (meta.find(";base64") != std::string::npos) {
        out = base64Decode(body);
    } else {
        const std::string decoded = percentDecode(body);
        out.assign(decoded.begin(), decoded.end());
    }
    return true;
}

} // namespace bro::util
