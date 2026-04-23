#pragma once
#include <string>
#include <unordered_map>

namespace bro::dom {

class Element;

class StyleProxy {
public:
    explicit StyleProxy(Element* owner = nullptr);

    std::string getProperty(const std::string& name) const;
    void setProperty(const std::string& name, const std::string& value);
    void removeProperty(const std::string& name);

    const std::string& cssText() const;
    void setCssText(const std::string& text);

    bool empty() const { return properties_.empty(); }
    size_t size() const { return properties_.size(); }
    const std::unordered_map<std::string, std::string>& properties() const { return properties_; }

    static std::string camelToKebab(const std::string& camel);
    static std::string kebabToCamel(const std::string& kebab);

private:
    void invalidateCssText() { cssTextDirty_ = true; }

    std::unordered_map<std::string, std::string> properties_;
    Element* owner_;
    mutable std::string cssTextCache_;
    mutable bool cssTextDirty_ = true;
};

} // namespace bro::dom
