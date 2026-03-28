#include "dom/style_proxy.h"
#include "dom/element.h"
#include <sstream>
#include <cctype>

namespace bro::dom {

StyleProxy::StyleProxy(Element* owner)
    : owner_(owner)
{
}

std::string StyleProxy::getProperty(const std::string& name) const {
    auto it = properties_.find(name);
    if (it != properties_.end()) {
        return it->second;
    }
    return {};
}

void StyleProxy::setProperty(const std::string& name, const std::string& value) {
    properties_[name] = value;
    if (owner_) {
        owner_->markDirty();
    }
}

void StyleProxy::removeProperty(const std::string& name) {
    if (properties_.erase(name) > 0) {
        if (owner_) {
            owner_->markDirty();
        }
    }
}

std::string StyleProxy::cssText() const {
    std::ostringstream oss;
    for (const auto& [key, val] : properties_) {
        oss << key << ": " << val << "; ";
    }
    std::string result = oss.str();
    // Remove trailing space if present
    if (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

void StyleProxy::setCssText(const std::string& text) {
    properties_.clear();

    // Parse "key: value; key: value; ..." format
    std::istringstream iss(text);
    std::string declaration;
    while (std::getline(iss, declaration, ';')) {
        auto colonPos = declaration.find(':');
        if (colonPos == std::string::npos) continue;

        std::string key = declaration.substr(0, colonPos);
        std::string val = declaration.substr(colonPos + 1);

        // Trim whitespace
        auto trimStart = [](std::string& s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(),
                [](unsigned char c) { return !std::isspace(c); }));
        };
        auto trimEnd = [](std::string& s) {
            s.erase(std::find_if(s.rbegin(), s.rend(),
                [](unsigned char c) { return !std::isspace(c); }).base(), s.end());
        };

        trimStart(key);
        trimEnd(key);
        trimStart(val);
        trimEnd(val);

        if (!key.empty() && !val.empty()) {
            properties_[key] = val;
        }
    }

    if (owner_) {
        owner_->markDirty();
    }
}

std::string StyleProxy::camelToKebab(const std::string& camel) {
    std::string result;
    for (size_t i = 0; i < camel.size(); ++i) {
        char c = camel[i];
        if (std::isupper(static_cast<unsigned char>(c))) {
            if (i > 0) {
                result += '-';
            }
            result += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else {
            result += c;
        }
    }
    return result;
}

std::string StyleProxy::kebabToCamel(const std::string& kebab) {
    std::string result;
    bool capitalizeNext = false;
    for (char c : kebab) {
        if (c == '-') {
            capitalizeNext = true;
        } else {
            if (capitalizeNext) {
                result += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                capitalizeNext = false;
            } else {
                result += c;
            }
        }
    }
    return result;
}

} // namespace bro::dom
