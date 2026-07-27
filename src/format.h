#pragma once

#include <cstdint>
#include <format>
#include <string>

namespace multidow {

inline std::string format_bytes(uint64_t b) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = b;
    int u = 0;
    while (v >= 1024 && u < 3) {
        v /= 1024;
        u++;
    }
    if (u == 3 && v < 10)
        return std::format("{:.3f} {}", v, units[u]);
    else
        return std::format("{:.1f} {}", v, units[u]);
}

inline std::string format_speed(uint64_t bps) {
    return format_bytes(bps) + "/s";
}

inline std::string make_bar(int w, double p) {
    int filled = (int)(p * w);
    if (filled > w) filled = w;
    std::string bar = "[";
    for (int i = 0; i < w; i++) bar += (i < filled) ? '#' : '.';
    bar += "]";
    return bar;
}

}  // namespace multidow
