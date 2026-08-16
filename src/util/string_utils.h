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

} // namespace bro::util
