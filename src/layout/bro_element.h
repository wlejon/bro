#pragma once

#include <litehtml.h>

namespace bro::layout {

/// Custom litehtml element that fixes runtime CSS class selector support.
///
/// litehtml's html_tag::apply_stylesheet has an optimization that skips
/// class-based CSS selectors when the class doesn't match at parse time.
/// This means selectors like `.active { background: blue }` are never
/// stored in m_used_styles for elements that don't initially have the
/// "active" class, so refresh_styles() can't find them later.
///
/// BroElement overrides apply_stylesheet to also store class-based
/// selectors that match the tag but not the current class, with
/// m_used = false. When the class changes at runtime and
/// refresh_styles() is called, these selectors are re-evaluated
/// and applied if they now match.
class BroElement : public litehtml::html_tag {
public:
    using html_tag::html_tag;

    void apply_stylesheet(const litehtml::css& stylesheet) override;
};

} // namespace bro::layout
