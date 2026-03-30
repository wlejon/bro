#pragma once

#include <litehtml/el_text.h>

namespace bro::layout {

/// el_text subclass with a public text setter.
/// Allows updating text content without destroying the render item.
class BroElText : public litehtml::el_text {
public:
    using el_text::el_text;

    void set_text(const char* text) {
        m_text = text ? text : "";
        m_use_transformed = false;
    }
};

} // namespace bro::layout
