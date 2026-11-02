// Matted Engine - lightweight CPU profiler (per-frame zone aggregation)
#pragma once
#include "engine/core/mtt_types.h"

namespace mtt::prof {

// Scoped timing zone. Register once via MTT_PROFILE_ZONE / MTT_PROFILE_FUNCTION.
struct ScopedZone {
    explicit ScopedZone(u32 zone_id);
    ~ScopedZone();
private:
    u32 id_;
    i64 t0_;
};

u32 zone_id(const char* name);            // interned id (stable across frames)
void frame_begin();                        // reset per-frame accumulation
void frame_end();                          // fold scopes into averages
void report_top(int n = 8, double* out_ms = nullptr); // log hottest zones
double zone_ms(u32 id);                    // avg ms/frame for zone
u64   zone_count(u32 id);

} // namespace mtt::prof

#define MTT_CONCAT_(a, b) a##b
#define MTT_CONCAT(a, b)  MTT_CONCAT_(a, b)
// __LINE__ (not __COUNTER__) so the id variable and the ScopedZone argument
// expand to the same name across both statements of the macro.
#define MTT_PROFILE_ZONE(name) \
    static const u32 MTT_CONCAT(mtt_zone_id_, __LINE__) = ::mtt::prof::zone_id(name); \
    ::mtt::prof::ScopedZone MTT_CONCAT(mtt_zone_sc_, __LINE__)(MTT_CONCAT(mtt_zone_id_, __LINE__))
#define MTT_PROFILE_FUNCTION() MTT_PROFILE_ZONE(__FUNCTION__)
