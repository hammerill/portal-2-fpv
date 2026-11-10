#pragma once

#include <string>

namespace paths {

// The folder this module (the loader or the core) was loaded from:
// "...\Portal 2\portal2\addons\portal2-fpv".
std::string mod_dir();

// fpv-data in the mod's folder: the developer triggers (cmd.txt, shot.txt,
// autotest.txt), status.txt and screenshots.
std::string data_dir();

}  // namespace paths
