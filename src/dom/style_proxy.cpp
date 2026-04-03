#include "dom/style_proxy.h"
#include "dom/element.h"
#include <algorithm>
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
    // Skip if value unchanged — avoids expensive style sync + layout
    auto it = properties_.find(name);
    if (it != properties_.end() && it->second == value) return;
    bool displayChanged = (name == "display");
    properties_[name] = value;
    invalidateCssText();
    if (owner_) {
        if (displayChanged) {
            owner_->markStructureDirty();
        } else {
            owner_->markDirty();
        }
    }
}

void StyleProxy::removeProperty(const std::string& name) {
    if (properties_.erase(name) > 0) {
        invalidateCssText();
        bool displayChanged = (name == "display");
        if (owner_) {
            if (displayChanged) {
                owner_->markStructureDirty();
            } else {
                owner_->markDirty();
            }
        }
    }
}

const std::string& StyleProxy::cssText() const {
    if (!cssTextDirty_) return cssTextCache_;
    cssTextCache_.clear();
    for (const auto& [key, val] : properties_) {
        cssTextCache_ += key;
        cssTextCache_ += ": ";
        cssTextCache_ += val;
        cssTextCache_ += "; ";
    }
    // Remove trailing space if present
    if (!cssTextCache_.empty() && cssTextCache_.back() == ' ') {
        cssTextCache_.pop_back();
    }
    cssTextDirty_ = false;
    return cssTextCache_;
}

void StyleProxy::setCssText(const std::string& text) {
    properties_.clear();
    invalidateCssText();

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
