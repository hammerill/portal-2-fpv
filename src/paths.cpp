#include "paths.h"

#include "platform.h"

namespace paths {

std::string mod_dir() {
    std::string d = platform::module_dir(reinterpret_cast<const void *>(&mod_dir));
    // macOS: the core runs from a copy in core-live/ (loader_mac.cpp)
    size_t slash = d.find_last_of("\\/");
    if (slash != std::string::npos && d.compare(slash + 1, std::string::npos, "core-live") == 0) d.resize(slash);
    return d;
}

std::string data_dir() {
    std::string d = mod_dir() + "/fpv-data";
    platform::make_dir(d);
    return d;
}

std::string game_dir() {
    std::string d = mod_dir();  // .../portal2/addons/portal2-fpv
    for (int i = 0; i < 2; i++) d = d.substr(0, d.find_last_of("\\/"));
    return d;
}

}  // namespace paths
