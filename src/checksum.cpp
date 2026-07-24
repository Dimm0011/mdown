#include "checksum.h"
#include <openssl/evp.h>
#include <format>
#include <fstream>
#include <iostream>

namespace multidow {

std::string sha256_file(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return "";

    const EVP_MD* md = EVP_sha256();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    char buf[8192];
    while (file.read(buf, sizeof(buf))) {
        if (EVP_DigestUpdate(ctx, buf, file.gcount()) != 1) {
            EVP_MD_CTX_free(ctx);
            return "";
        }
    }
    if (file.gcount() > 0) {
        if (EVP_DigestUpdate(ctx, buf, file.gcount()) != 1) {
            EVP_MD_CTX_free(ctx);
            return "";
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }
    EVP_MD_CTX_free(ctx);

    std::string result;
    result.reserve(hash_len * 2);
    for (unsigned int i = 0; i < hash_len; i++) result += std::format("{:02x}", hash[i]);
    return result;
}

bool verify_checksum(const std::string& filepath, const std::string& expected) {
    std::string actual = sha256_file(filepath);
    if (actual.empty()) {
        std::cerr << "Cannot compute checksum" << std::endl;
        return false;
    }
    return actual == expected;
}

}  // namespace multidow
