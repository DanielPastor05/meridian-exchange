#pragma once
#include <istream>
#include <string>

namespace meridian {
// Discard excess bytes without growing the buffer, then resume at the next line.
inline bool read_bounded_line(std::istream& input, std::string& line, bool& exceeded,
                              std::size_t maximum = 4096) {
    line.clear();
    exceeded = false;
    char c{};
    bool any = false;
    while (input.get(c)) {
        any = true;
        if (c == '\n') break;
        if (line.size() < maximum) line.push_back(c);
        else exceeded = true;
    }
    return any;
}
}
