#pragma once

#include <string>
#include <cstdint>
#include <sstream>
#include <iomanip>

namespace multidow {

inline std::string format_bytes(uint64_t b) {
    const char* units[] = {"B", "KB", "MB", "GB"};
    double v = b;
    int u = 0;
    while (v >= 1024 && u < 3) { v /= 1024; u++; }
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1) << v << " " << units[u];
    return ss.str();
}

inline std::string format_speed(uint64_t bps) {
    return format_bytes(bps) + "/s";
}

inline std::string make_bar(int w, double p) {
    int filled = (int)(p * w);
    if (filled > w) filled = w;
    std::string bar = "[";
    for (int i = 0; i < w; i++)
        bar += (i < filled) ? '#' : '.';
    bar += "]";
    return bar;
}

}
