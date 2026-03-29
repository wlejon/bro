#include "layout/bro_element.h"

namespace bro::layout {

void BroElement::apply_stylesheet(const litehtml::css& stylesheet) {
    for (const auto& sel : stylesheet.selectors()) {
        const auto& r = sel->m_right;

        // Keep the tag optimization: skip selectors for different tags.
        if (r.m_tag != litehtml::star_id && r.m_tag != m_tag)
            continue;

        // For class-based selectors that don't match the current classes,
        // still store them (with m_used=false) so refresh_styles() can
        // re-evaluate them after a runtime className change.
        bool class_mismatch = false;
        if (!r.m_attrs.empty()) {
            const auto& attr = r.m_attrs[0];
            if (attr.type == litehtml::select_class && !litehtml::contains(m_classes, attr.name)) {
                class_mismatch = true;
            }
        }

        int apply = select(*sel, false);

        if (apply != litehtml::select_no_match) {
            // Selector matches structurally — store and apply as normal.
            auto us = std::make_unique<litehtml::used_selector>(sel, false);

            if (sel->is_media_valid()) {
                if (apply & litehtml::select_match_pseudo_class) {
                    if (select(*sel, true)) {
                        add_style(*sel->m_style);
                        us->m_used = true;
                    }
                } else {
                    add_style(*sel->m_style);
                    us->m_used = true;
                }
            }
            m_used_styles.push_back(std::move(us));
        } else if (class_mismatch) {
            // Selector didn't match because of a class mismatch.
            // Store it anyway so refresh_styles() can find it later
            // when the element's class changes at runtime.
            m_used_styles.push_back(
                std::make_unique<litehtml::used_selector>(sel, false));
        }
    }

    // Recurse into children.
    for (auto& el : m_children) {
        if (el->css().get_display() != litehtml::display_inline_text) {
            el->apply_stylesheet(stylesheet);
        }
    }
}

} // namespace bro::layout
