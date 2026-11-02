#include "engine/core/log.h"
#include <cstdarg>
#include <chrono>
#include <ctime>

namespace mtt::log {

static Level g_level = Level::Info;

void set_level(Level lvl) { g_level = lvl; }

void write(Level lvl, const char* fmt, ...) {
    if (lvl < g_level) return;
    const char* tag = "INFO ";
    switch (lvl) {
        case Level::Debug: tag = "DEBUG"; break;
        case Level::Info:  tag = "INFO "; break;
        case Level::Warn:  tag = "WARN "; break;
        case Level::Error: tag = "ERROR"; break;
    }
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);

    va_list args;
    va_start(args, fmt);
    std::fprintf(stderr, "[matted %s %s] ", stamp, tag);
    std::vfprintf(stderr, fmt, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

} // namespace mtt::log
