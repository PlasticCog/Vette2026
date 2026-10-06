#pragma once
// Fitting text into the launch menu's lines.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vette::ui {

// Splits text into lines of at most `width` characters at spaces.
inline std::vector<std::string> wrap(std::string_view text, size_t width) {
    std::vector<std::string> lines;
    std::string line;
    while (!text.empty()) {
        const size_t end = text.find(' ');
        const std::string_view word = text.substr(0, end);
        if (!line.empty() && line.size() + 1 + word.size() > width) {
            lines.push_back(line);
            line.clear();
        }
        line += line.empty() ? std::string(word) : " " + std::string(word);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

// Fits text into `chars` characters by dropping the start (paths keep their informative end).
inline std::string fit_left(std::string s, size_t chars) {
    if (s.size() <= chars || chars < 4)
        return s;
    return "..." + s.substr(s.size() - (chars - 3));
}

}  // namespace vette::ui
