#pragma once

#include <string>
#include <cstdint>

namespace multidow {

std::string sha256_file(const std::string& filepath);
bool verify_checksum(const std::string& filepath, const std::string& expected);

}
