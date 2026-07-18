#pragma once

#include "dom/node.h"
#include "dom/text_node.h"
#include "dom/comment_node.h"

#include <algorithm>
#include <string>

// ===========================================================================
// UTF-8 (internal) ⇄ UTF-16 (JS) text-offset conversion.
//
// bro stores every DOM string as UTF-8 and every internal text offset —
// layout, hit-testing, caret placement, IME composition, Range/Selection
// endpoints — as a BYTE index into that UTF-8. The web platform specifies all
// string offsets in UTF-16 code units, because JS strings are UTF-16: the
// oracle for `text.length` / `substringData(i, n)` / `range.startOffset` is
// what the equivalent JS string would report.
//
// The two domains agree only on ASCII. A 2-byte sequence (accented Latin) and
// a 3-byte sequence (CJK) are each ONE UTF-16 unit; a 4-byte sequence (astral
// — emoji, U+10000+) is TWO (a surrogate pair). So every JS-visible offset is
// converted here, at the binding boundary, and the engine stays byte-domain.
//
// Conversion happens exactly once per crossing. Internal consumers read the
// stored byte offsets directly off the C++ Range/Selection objects and must
// never see a UTF-16 value.
// ===========================================================================

namespace bro::dom {

// Length in bytes of the UTF-8 sequence starting with lead byte `c`. An
// invalid lead byte counts as a 1-byte / 1-unit character so walks terminate.
inline int utf8SeqLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// Byte offset into `s` → UTF-16 code-unit index. Offsets landing mid-sequence
// resolve to the preceding character boundary. Clamped to [0, length].
inline int utf8ByteToUtf16(const std::string& s, int byte) {
    const int n = static_cast<int>(s.size());
    byte = std::clamp(byte, 0, n);
    int i = 0, units = 0;
    while (i < byte) {
        const int len = utf8SeqLen(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
        if (i + len > byte) break;  // mid-sequence → preceding boundary
        units += (len == 4) ? 2 : 1;
        i += len;
    }
    return units;
}

// UTF-16 code-unit index → byte offset into `s`. An index landing between the
// two units of a surrogate pair resolves to the preceding character boundary
// (the byte domain cannot name the middle of a code point). Clamped.
inline int utf16ToUtf8Byte(const std::string& s, int u16) {
    const int n = static_cast<int>(s.size());
    if (u16 < 0) u16 = 0;
    int i = 0, units = 0;
    while (i < n && units < u16) {
        const int len = utf8SeqLen(static_cast<unsigned char>(s[static_cast<size_t>(i)]));
        const int u = (len == 4) ? 2 : 1;
        if (units + u > u16) break;  // mid-astral → preceding boundary
        units += u;
        i += std::min(len, n - i);
    }
    return i;
}

// Total length of `s` in UTF-16 code units.
inline int utf16Length(const std::string& s) {
    return utf8ByteToUtf16(s, static_cast<int>(s.size()));
}

// The character data a node's text offsets index, or nullptr when the node
// isn't a CharacterData node (offsets on Element/Document containers are
// child indices, which are domain-free and must NOT be converted).
inline const std::string* characterDataOf(const Node* node) {
    if (!node) return nullptr;
    if (node->nodeType() == NodeType::Text)
        return &static_cast<const TextNode*>(node)->data();
    if (node->nodeType() == NodeType::Comment)
        return &static_cast<const CommentNode*>(node)->data();
    return nullptr;
}

// Boundary-point conversion for (container, offset) pairs — Range and
// Selection endpoints. Child-index offsets pass through untouched.
inline int nodeOffsetToUtf16(const Node* container, int byteOffset) {
    if (const std::string* d = characterDataOf(container))
        return utf8ByteToUtf16(*d, byteOffset);
    return byteOffset;
}

inline int nodeOffsetToBytes(const Node* container, int utf16Offset) {
    if (const std::string* d = characterDataOf(container))
        return utf16ToUtf8Byte(*d, utf16Offset);
    return utf16Offset;
}

} // namespace bro::dom
