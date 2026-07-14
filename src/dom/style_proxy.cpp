#include "dom/style_proxy.h"
#include "dom/element.h"
#include "dom/document.h"
#include "css/properties.h"
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

// `inherit` on a property that does not normally inherit defeats the scoped
// restyle (see Document::noteForcedInherit). Expand shorthands before deciding:
// `font: inherit` is only inherited longhands, `background: inherit` is not.
void StyleProxy::noteIfForcedInherit(const std::string& name) {
    if (!owner_ || !owner_->document()) return;
    for (auto& e : htmlayout::css::expandShorthand(name, "inherit")) {
        if (!htmlayout::css::isInherited(e.property)) {
            owner_->document()->noteForcedInherit();
            return;
        }
    }
}

void StyleProxy::setProperty(const std::string& name, const std::string& value) {
    // CSSOM: setting a property to the empty string removes it (so the cascade
    // falls back to author stylesheets). This is what `el.style.display = ''`
    // and setProperty(name, '') both mean — without it the empty declaration
    // lingers and shadows a stylesheet rule (e.g. a `.panel { display:flex }`).
    {
        size_t b = value.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) { removeProperty(name); return; }
    }
    // Skip if value unchanged — avoids expensive style sync + layout
    auto it = properties_.find(name);
    if (it != properties_.end() && it->second == value) return;
    properties_[name] = value;
    invalidateCssText();
    // `el.style.border = 'inherit'` ties this element to a parent property that
    // does not inherit, which is the one thing the scoped restyle cannot see.
    // (`font: inherit` does not count — every longhand it expands to inherits.)
    if (value == "inherit") noteIfForcedInherit(name);
    if (owner_) {
        // Paint-dirty, not layout-dirty: an inline-style write is a change to a
        // style *input*, and the cascade has not run yet, so we cannot know here
        // whether geometry moved. resolveStyles() diffs the new computed style
        // against the old one and promotes to a real layout only when a
        // layout-affecting property actually changed — so `style.opacity = x`
        // repaints, while `style.width = x` reflows. markDirty() would
        // pre-declare the reflow and relayout this element's whole subtree on
        // every paint-only write.
        //
        // `display` needs nothing special either: the layout tree's shape is a
        // function of the DOM alone (a display:none element still gets a layout
        // node, which layout zero-sizes), so a display change is a style change
        // like any other and the diff promotes it.
        owner_->markStyleDirty();
    }
}

void StyleProxy::removeProperty(const std::string& name) {
    if (properties_.erase(name) > 0) {
        invalidateCssText();
        if (owner_) owner_->markStyleDirty();   // see setProperty
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
            if (val == "inherit") noteIfForcedInherit(key);   // see setProperty
        }
    }

    if (owner_) owner_->markStyleDirty();   // see setProperty
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
