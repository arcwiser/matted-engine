// Matted Engine - math library (header-only)
// Column-major mat4 (OpenGL style). Vectors are plain floats; the optimizing
// compiler auto-vectorizes the hot paths (/O2 /fp:fast).
#pragma once
#include <cmath>
#include "engine/core/mtt_types.h"

namespace mtt {

constexpr float kPi      = 3.14159265358979323846f;
constexpr float kTwoPi   = kPi * 2.0f;
constexpr float kDegToRad= kPi / 180.0f;
constexpr float kRadToDeg= 180.0f / kPi;

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float lerpf (float a, float b, float t)   { return a + (b - a) * t; }
inline float saturatef(float v)                  { return clampf(v, 0.0f, 1.0f); }
inline float minf(float a, float b)              { return a < b ? a : b; }
inline float maxf(float a, float b)              { return a > b ? a : b; }
inline i32   mini(i32 a, i32 b)                  { return a < b ? a : b; }
inline u32   minu(u32 a, u32 b)                  { return a < b ? a : b; }
inline u32   maxu(u32 a, u32 b)                  { return a > b ? a : b; }

// ---------------------------------------------------------------- vec2
struct vec2 {
    float x = 0, y = 0;
    vec2() = default;
    vec2(float s) : x(s), y(s) {}
    vec2(float x_, float y_) : x(x_), y(y_) {}
    vec2 operator+(const vec2& o) const { return {x + o.x, y + o.y}; }
    vec2 operator-(const vec2& o) const { return {x - o.x, y - o.y}; }
    vec2 operator*(float s) const { return {x * s, y * s}; }
};

// ---------------------------------------------------------------- vec3
struct vec3 {
    float x = 0, y = 0, z = 0;
    vec3() = default;
    vec3(float s) : x(s), y(s), z(s) {}
    vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    vec3 operator+(const vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    vec3 operator-(const vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    vec3 operator-()             const { return {-x, -y, -z}; }
    vec3 operator*(const vec3& o) const { return {x * o.x, y * o.y, z * o.z}; }
    vec3 operator*(float s)       const { return {x * s, y * s, z * s}; }
    vec3 operator/(float s)       const { return {x / s, y / s, z / s}; }
    vec3& operator+=(const vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    vec3& operator-=(const vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    vec3& operator*=(float s)       { x *= s; y *= s; z *= s; return *this; }

    static const vec3 zero()     { return {0, 0, 0}; }
    static const vec3 one()      { return {1, 1, 1}; }
    static const vec3 up()       { return {0, 1, 0}; }
    static const vec3 down()     { return {0, -1, 0}; }
    static const vec3 right()    { return {1, 0, 0}; }
    static const vec3 forward()  { return {0, 0, -1}; }
};

inline vec3  operator*(float s, const vec3& v) { return v * s; }
inline float dot(const vec3& a, const vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline vec3  cross(const vec3& a, const vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length_sq(const vec3& v) { return dot(v, v); }
inline float length(const vec3& v)    { return std::sqrt(length_sq(v)); }
inline float dist_sq(const vec3& a, const vec3& b) { return length_sq(a - b); }
inline vec3  normalize(const vec3& v) {
    float l = length(v);
    return l > 1e-12f ? v / l : vec3(0);
}
inline vec3 lerp(const vec3& a, const vec3& b, float t) { return a + (b - a) * t; }
inline vec3 abs(const vec3& v) { return {std::fabs(v.x), std::fabs(v.y), std::fabs(v.z)}; }
inline vec3 minv(const vec3& a, const vec3& b) {
    return {minf(a.x, b.x), minf(a.y, b.y), minf(a.z, b.z)};
}
inline vec3 maxv(const vec3& a, const vec3& b) {
    return {maxf(a.x, b.x), maxf(a.y, b.y), maxf(a.z, b.z)};
}
inline vec3 reflect(const vec3& v, const vec3& n) { return v - n * (2.0f * dot(v, n)); }

// ---------------------------------------------------------------- vec4
struct vec4 {
    float x = 0, y = 0, z = 0, w = 0;
    vec4() = default;
    vec4(float s) : x(s), y(s), z(s), w(s) {}
    vec4(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}
    vec4(const vec3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
    vec3 xyz() const { return {x, y, z}; }
    vec4 operator+(const vec4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
    vec4 operator-(const vec4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
};

// ---------------------------------------------------------------- quat
struct quat {
    float x = 0, y = 0, z = 0, w = 1; // (x,y,z) = axis*sin(theta/2), w = cos(theta/2)
    quat() = default;
    quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    static quat identity() { return quat(0, 0, 0, 1); }
    static quat from_axis_angle(const vec3& axis, float angle_rad) {
        float a = angle_rad * 0.5f, s = std::sin(a);
        return quat(axis.x * s, axis.y * s, axis.z * s, std::cos(a));
    }
    quat operator*(const quat& b) const {
        return quat(
            w * b.x + x * b.w + y * b.z - z * b.y,
            w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w,
            w * b.w - x * b.x - y * b.y - z * b.z);
    }
    quat conjugate() const { return quat(-x, -y, -z, w); }
    quat normalized() const {
        float l = std::sqrt(x * x + y * y + z * z + w * w);
        return l > 1e-12f ? quat(x / l, y / l, z / l, w / l) : identity();
    }
    vec3 rotate(const vec3& v) const { // q * v * q*
        quat t = *this * quat(v.x, v.y, v.z, 0) * conjugate();
        return {t.x, t.y, t.z};
    }
    vec3 rotate_inv(const vec3& v) const { return conjugate().rotate(v); } // R^T * v
};

// ---------------------------------------------------------------- mat3 (3x3 column-major float[9])
inline void quat_to_mat3(const quat& q, float out[9]) {
    float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    // columns
    out[0] = 1 - 2 * (yy + zz); out[1] = 2 * (xy + wz);     out[2] = 2 * (xz - wy);
    out[3] = 2 * (xy - wz);     out[4] = 1 - 2 * (xx + zz); out[5] = 2 * (yz + wx);
    out[6] = 2 * (xz + wy);     out[7] = 2 * (yz - wx);     out[8] = 1 - 2 * (xx + yy);
}
inline vec3 mat3_mul(const float m[9], const vec3& v) {
    return {m[0] * v.x + m[3] * v.y + m[6] * v.z,
            m[1] * v.x + m[4] * v.y + m[7] * v.z,
            m[2] * v.x + m[5] * v.y + m[8] * v.z};
}

// ---------------------------------------------------------------- mat4 (column-major m[col*4+row])
struct mat4 {
    float m[16];

    static mat4 identity() {
        mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
        return r;
    }
    // r = a * b (column-major)
    static mat4 mul(const mat4& a, const mat4& b) {
        mat4 r{};
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < 4; ++i) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += a.m[k * 4 + i] * b.m[c * 4 + k];
                r.m[c * 4 + i] = s;
            }
        return r;
    }
    vec4 row(int i) const { return {m[i], m[4 + i], m[8 + i], m[12 + i]}; }

    static mat4 perspective(float fov_y_rad, float aspect, float zn, float zf) {
        mat4 r{};
        const float f = 1.0f / std::tan(fov_y_rad * 0.5f);
        r.m[0]  = f / aspect;
        r.m[5]  = f;
        r.m[10] = zf / (zn - zf);
        r.m[11] = -1.0f;
        r.m[14] = zn * zf / (zn - zf);
        return r;
    }
    static mat4 look_at(const vec3& eye, const vec3& at, const vec3& up) {
        vec3 f = normalize(at - eye);          // forward
        vec3 s = normalize(cross(f, up));      // right
        vec3 u = cross(s, f);                  // true up
        mat4 r = identity();
        r.m[0] = s.x;  r.m[4] = s.y;  r.m[8]  = s.z;
        r.m[1] = u.x;  r.m[5] = u.y;  r.m[9]  = u.z;
        r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
        r.m[12] = -dot(s, eye);
        r.m[13] = -dot(u, eye);
        r.m[14] =  dot(f, eye);
        return r;
    }
    static mat4 trs(const vec3& pos, const quat& q, const vec3& scl) {
        float r[9];
        quat_to_mat3(q, r);
        mat4 out = identity();
        out.m[0] = r[0] * scl.x; out.m[1] = r[1] * scl.x; out.m[2]  = r[2] * scl.x;
        out.m[4] = r[3] * scl.y; out.m[5] = r[4] * scl.y; out.m[6]  = r[5] * scl.y;
        out.m[8] = r[6] * scl.z; out.m[9] = r[7] * scl.z; out.m[10] = r[8] * scl.z;
        out.m[12] = pos.x; out.m[13] = pos.y; out.m[14] = pos.z;
        return out;
    }
    vec3 transform_point(const vec3& v) const {
        return {m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12],
                m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13],
                m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]};
    }
    vec3 transform_dir(const vec3& v) const {
        return {m[0] * v.x + m[4] * v.y + m[8]  * v.z,
                m[1] * v.x + m[5] * v.y + m[9]  * v.z,
                m[2] * v.x + m[6] * v.y + m[10] * v.z};
    }
    vec4 transform_vec4(const vec4& v) const {
        return {m[0] * v.x + m[4] * v.y + m[8]  * v.z + m[12] * v.w,
                m[1] * v.x + m[5] * v.y + m[9]  * v.z + m[13] * v.w,
                m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14] * v.w,
                m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15] * v.w};
    }
};

// ---------------------------------------------------------------- aabb
struct aabb {
    vec3 mn{ kInvalidf(),  kInvalidf(),  kInvalidf() };
    vec3 mx{ -kInvalidf(), -kInvalidf(), -kInvalidf() };

    static constexpr float kInvalidf() { return 1e30f; }
    static aabb empty() { return aabb{}; }
    bool is_empty() const { return mn.x > mx.x; }

    void expand_point(const vec3& p) { mn = minv(mn, p); mx = maxv(mx, p); }
    void expand_aabb(const aabb& b)  { mn = minv(mn, b.mn); mx = maxv(mx, b.mx); }
    vec3 center() const { return (mn + mx) * 0.5f; }
    vec3 extents() const { return (mx - mn) * 0.5f; }
    bool intersects(const aabb& b) const {
        if (is_empty() || b.is_empty()) return false;
        return mn.x <= b.mx.x && mx.x >= b.mn.x &&
               mn.y <= b.mx.y && mx.y >= b.mn.y &&
               mn.z <= b.mx.z && mx.z >= b.mn.z;
    }
    // World bounds of this (local) aabb under pos/rot/scale (conservative).
    aabb transformed(const vec3& pos, const quat& rot, const vec3& scl) const {
        vec3 c  = center() * scl;
        vec3 e  = abs(extents() * scl);
        float r[9];
        quat_to_mat3(rot, r);
        vec3 rc = mat3_mul(r, c);
        vec3 re{ std::fabs(r[0]) * e.x + std::fabs(r[3]) * e.y + std::fabs(r[6]) * e.z,
                 std::fabs(r[1]) * e.x + std::fabs(r[4]) * e.y + std::fabs(r[7]) * e.z,
                 std::fabs(r[2]) * e.x + std::fabs(r[5]) * e.y + std::fabs(r[8]) * e.z };
        aabb out;
        out.expand_point(pos + rc - re);
        out.expand_point(pos + rc + re);
        return out;
    }
};

// ---------------------------------------------------------------- plane & frustum
struct plane {
    vec3 n{0, 1, 0};
    float d = 0; // n . p + d = 0 on plane; d<0 in front
    float distance(const vec3& p) const { return dot(n, p) + d; }
};

// 6 planes built from view-projection (Gribb & Hartmann). n points inside the frustum.
struct frustum {
    plane pl[6];

    void build_from_viewproj(const mat4& vp) {
        // rows of vp (column-major): row_i(j) = m[j*4+i]
        const auto row = [&](int i) {
            return vec4(vp.m[i], vp.m[4 + i], vp.m[8 + i], vp.m[12 + i]);
        };
        vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
        const vec4 rows[6][2] = {
            {r3 + r0, {}}, {r3 - r0, {}},   // left, right
            {r3 + r1, {}}, {r3 - r1, {}},   // bottom, top
            {r3 + r2, {}}, {r3 - r2, {}},   // near, far
        };
        for (int i = 0; i < 6; ++i) {
            vec4 p = rows[i][0];
            float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
            if (len > 1e-12f) p = vec4(p.x / len, p.y / len, p.z / len, p.w / len);
            pl[i].n = {p.x, p.y, p.z};
            pl[i].d = p.w;
        }
    }
    // Positive-vertex AABB test.
    bool intersects(const aabb& b) const {
        for (int i = 0; i < 6; ++i) {
            const plane& p = pl[i];
            vec3 pv{ p.n.x >= 0 ? b.mx.x : b.mn.x,
                     p.n.y >= 0 ? b.mx.y : b.mn.y,
                     p.n.z >= 0 ? b.mx.z : b.mn.z };
            if (p.distance(pv) < 0) return false;
        }
        return true;
    }
};

} // namespace mtt
