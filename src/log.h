// The mod's log, portal2-fpv.log next to the DLLs. The loader starts it
// afresh; the core and libfpv append to it.
#pragma once

namespace fpvlog {

void write(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace fpvlog
