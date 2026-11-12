#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

#include "paths.h"
#include "platform.h"

namespace fpvlog {

void write(const char *fmt, ...) {
    static std::mutex mutex;
    // The loader and the core both write this file, so both append; only the
    // loader starts it afresh.
    static FILE *file = [] {
        std::string path = paths::mod_dir() + "/portal2-fpv.log";
#ifndef FPVLOG_APPEND
        if (FILE *f = std::fopen(path.c_str(), "w")) std::fclose(f);
#endif
        return std::fopen(path.c_str(), "a");
    }();
    if (!file) return;

    std::lock_guard<std::mutex> lock(mutex);
    int h, m, sec, ms;
    platform::local_time(h, m, sec, ms);
    std::fprintf(file, "%02d:%02d:%02d.%03d ", h, m, sec, ms);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(file, fmt, args);
    va_end(args);
    std::fputc('\n', file);
    std::fflush(file);
}

}  // namespace fpvlog
