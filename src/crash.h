// Fatal exceptions to fpv-data/crash.txt (appended, so it survives the game's
// restart): code, address as module+offset, registers, and the return
// addresses on the stack that fall in a module.
#pragma once

namespace crash {

void install();
void uninstall();
void note(const char *where);  // the last step reached, logged with a crash

}  // namespace crash
