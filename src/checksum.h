#pragma once

#include <cstdint>
#include <string>

namespace multidow {

std::string sha256_file(const std::string& filepath);
bool verify_checksum(const std::string& filepath, const std::string& expected);

}  // namespace multidow
