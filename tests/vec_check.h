// Matted Engine - tests: vec3 near-equality helper (kept out of the generic
// framework so it stays engine-independent).
#pragma once
#include "test_framework.h"
#include "engine/core/math/math.h"

inline bool v3_near(const mtt::vec3& a, const mtt::vec3& b, float eps,
                    const char* what, const char* file, int line) {
    const bool ok = std::fabs(a.x - b.x) <= eps &&
                    std::fabs(a.y - b.y) <= eps &&
                    std::fabs(a.z - b.z) <= eps;
    return mtttest::check(ok, what, file, line);
}

#define MTT_NEAR3(a, b, eps) v3_near((a), (b), (eps), #a " ~= " #b, __FILE__, __LINE__)
