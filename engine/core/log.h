// Matted Engine - logging
#pragma once
#include <cstdio>
#include "engine/core/mtt_types.h"

namespace mtt::log {

enum class Level : u8 { Debug = 0, Info, Warn, Error };

void set_level(Level lvl);
void write(Level lvl, const char* fmt, ...);

inline void debug(const char* fmt, ...) { write(Level::Debug, fmt); }
inline void info (const char* fmt, ...) { write(Level::Info,  fmt); }
inline void warn (const char* fmt, ...) { write(Level::Warn,  fmt); }
inline void error(const char* fmt, ...) { write(Level::Error, fmt); }

} // namespace mtt::log
