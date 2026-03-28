#pragma once

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

} // namespace bro::util
