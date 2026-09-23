#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace bro::util {

std::string trim(const std::string& str);
std::vector<std::string> split(const std::string& str, char delimiter);
std::string toLower(const std::string& str);
std::string toUpper(const std::string& str);
bool startsWith(const std::string& str, const std::string& prefix);
bool endsWith(const std::string& str, const std::string& suffix);
std::string replace(const std::string& str, const std::string& from, const std::string& to);

/// Decode standard base64. Skips whitespace and padding, and ignores any
/// character outside the alphabet rather than failing — callers here feed it
/// data: URL bodies, where a stray newline is normal and a hard error would
/// only turn a renderable image into a blank one.
std::vector<uint8_t> base64Decode(const std::string& s);

/// Encode standard base64, with padding and no line breaks — the form a
/// `data:` URL wants, which is what canvas.toDataURL() hands back.
std::string base64Encode(const uint8_t* data, size_t len);

/// An 8-bit alpha as the shortest decimal that maps back to the same byte
/// (128 -> "0.5", 77 -> "0.3", 0 -> "0", 255 -> "1") — how browsers print
/// the alpha of a color they keep at 8 bits.
std::string alphaToString(uint8_t a);

/// HTML's "serialization of a color" (canvas fillStyle / strokeStyle /
/// shadowColor): "#rrggbb" in lowercase hex when opaque, otherwise
/// "rgba(r, g, b, a)".
std::string serializeCanvasColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

/// CSSOM's resolved value of a color (getComputedStyle): "rgb(r, g, b)" when
/// opaque, otherwise "rgba(r, g, b, a)".
std::string serializeCssColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

} // namespace bro::util
