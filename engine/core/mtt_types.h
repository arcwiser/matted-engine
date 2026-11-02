// Matted Engine - core fixed-width types & helpers
#pragma once
#include <cstdint>
#include <cstddef>

namespace mtt {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

constexpr u32 kInvalid32 = 0xFFFFFFFFu;

// Generational entity handle used by the ECS (20-bit index, 12-bit version).
struct Entity {
    u32 id = kInvalid32;
    bool valid() const { return id != kInvalid32; }
    u32 index()  const { return id & 0xFFFFFu; }
    u32 version()const { return (id >> 20) & 0xFFFu; }
    bool operator==(const Entity& o) const { return id == o.id; }
    bool operator!=(const Entity& o) const { return id != o.id; }
};

inline constexpr u32 kEntityIndexBits = 20;
inline constexpr u32 kEntityIndexMask = (1u << kEntityIndexBits) - 1;

} // namespace mtt
