#include "renut_engine/mods/mod_device.h"

namespace renut::mods {

std::string PathKey(std::string_view path) {
    std::string key;
    key.reserve(path.size());
    for (char c : path) {
        if (c == '\\')
            c = '/';
        else if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
        if (c == '/' && (key.empty() || key.back() == '/'))
            continue;
        key += c;
    }
    while (!key.empty() && key.back() == '/')
        key.pop_back();
    return key;
}

} // namespace renut::mods
