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

    std::string cssText() const;
    void setCssText(const std::string& text);

    static std::string camelToKebab(const std::string& camel);
    static std::string kebabToCamel(const std::string& kebab);

private:
    std::unordered_map<std::string, std::string> properties_;
    Element* owner_;
};

} // namespace bro::dom
