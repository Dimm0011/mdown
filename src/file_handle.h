#pragma once

#include <cstdio>
#include <string>
#include <utility>

namespace multidow {

class FileHandle {
   public:
    FileHandle() = default;
    explicit FileHandle(FILE* f) noexcept : f_(f) {}
    FileHandle(const std::string& path, const char* mode) noexcept
        : f_(fopen(path.c_str(), mode)) {}

    ~FileHandle() {
        close();
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    FileHandle(FileHandle&& o) noexcept : f_(std::exchange(o.f_, nullptr)) {}
    FileHandle& operator=(FileHandle&& o) noexcept {
        if (this != &o) {
            close();
            f_ = std::exchange(o.f_, nullptr);
        }
        return *this;
    }

    void close() {
        if (f_) {
            fclose(f_);
            f_ = nullptr;
        }
    }
    [[nodiscard]] bool is_open() const {
        return f_ != nullptr;
    }
    [[nodiscard]] FILE* get() const {
        return f_;
    }
    [[nodiscard]] explicit operator bool() const {
        return f_ != nullptr;
    }

   private:
    FILE* f_ = nullptr;
};

}  // namespace multidow
