// Matted Engine - narrowphase: sphere/box/plane collision + SAT box-box with
// reference-face clipping. Manifold normal convention: points from A to B.
// Solver applies +impulse*normal to B and -impulse*normal to A.
#include "engine/physics/physics.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mtt::physics {

static inline vec3 body_axis(const float r[9], int i) {
    return { r[i * 3 + 0], r[i * 3 + 1], r[i * 3 + 2] }; // column i = axis i
}

// component accessor (vec3 has no operator[] by design)
static inline float comp(const vec3& v, int i) {
    return i == 0 ? v.x : i == 1 ? v.y : v.z;
}

// ---------------------------------------------------------------- sphere-sphere
static void collide_sphere_sphere(const Body& A, const Body& B, Manifold& m) {
    const vec3 d = B.pos - A.pos;
    const float dist = length(d);
    const float rsum = A.shape.radius + B.shape.radius;
    if (dist >= rsum) return;
    const vec3 n = dist > 1e-9f ? d / dist : vec3(0, 1, 0);
    m.normal = n;
    ContactPoint& p = m.pts[m.npts++];
    p.pos = A.pos + n * (A.shape.radius - 0.5f * (rsum - dist));
    p.pen = rsum - dist;
    p.feature = 0;
}

// ---------------------------------------------------------------- sphere-box (A=sphere, B=box)
static void collide_sphere_box(const Body& A, const Body& B, Manifold& m) {
    float R[9];
    quat_to_mat3(B.rot, R);
    const vec3 rel = A.pos - B.pos;
    const vec3 ax0 = body_axis(R, 0), ax1 = body_axis(R, 1), ax2 = body_axis(R, 2);
    const vec3 local = { dot(rel, ax0), dot(rel, ax1), dot(rel, ax2) };
    const vec3& h = B.shape.half;

    vec3 cl = { clampf(local.x, -h.x, h.x), clampf(local.y, -h.y, h.y), clampf(local.z, -h.z, h.z) };
    float dist;
    if (cl.x == local.x && cl.y == local.y && cl.z == local.z) {
        // center inside the box: exit through the nearest face
        float best = h.x - local.x; vec3 bn(1, 0, 0);
        float d2 = local.x + h.x; if (d2 < best) { best = d2; bn = vec3(-1, 0, 0); }
        d2 = h.y - local.y;       if (d2 < best) { best = d2; bn = vec3(0, 1, 0); }
        d2 = local.y + h.y;       if (d2 < best) { best = d2; bn = vec3(0, -1, 0); }
        d2 = h.z - local.z;       if (d2 < best) { best = d2; bn = vec3(0, 0, 1); }
        d2 = local.z + h.z;       if (d2 < best) { best = d2; bn = vec3(0, 0, -1); }
        dist = -best;
        cl = local + bn * best;
    } else {
        const vec3 diff = local - cl;
        dist = length(diff);
        if (dist > 1e-9f) cl = local - diff / dist * 0.0f; // keep cl as surface point
    }

    const float r = A.shape.radius;
    if (dist >= r) return;

    // n_local points box -> sphere; manifold normal A->B = sphere->box = -n_local
    vec3 n_local;
    if (dist > 1e-9f) {
        const vec3 diff = local - cl;
        n_local = diff / dist;
    } else {
        n_local = (cl - local) / (length(cl - local) + 1e-9f); // inside case
    }
    m.normal = -(ax0 * n_local.x + ax1 * n_local.y + ax2 * n_local.z);

    ContactPoint& p = m.pts[m.npts++];
    p.pos = B.pos + ax0 * cl.x + ax1 * cl.y + ax2 * cl.z; // point on box surface
    p.pen = r - dist;
    p.feature = 1;
}

// ---------------------------------------------------------------- plane helpers (plane = B)
static void plane_world(const Body& P, vec3& n, float& d) {
    n = P.rot.rotate(P.shape.plane_n);
    d = P.shape.plane_d - dot(n, P.pos);
}

static void collide_sphere_plane(const Body& A, const Body& B, Manifold& m) {
    vec3 pn; float pd;
    plane_world(B, pn, pd);
    const float dist = dot(pn, A.pos) + pd;
    if (dist >= A.shape.radius) return;
    m.normal = -pn; // A->B (into the plane); -impulse pushes sphere along +pn
    ContactPoint& p = m.pts[m.npts++];
    p.pos = A.pos - pn * dist;
    p.pen = A.shape.radius - dist;
    p.feature = 0;
}

static void collide_box_plane(const Body& A, const Body& B, Manifold& m) {
    vec3 pn; float pd;
    plane_world(B, pn, pd);
    float R[9];
    quat_to_mat3(A.rot, R);
    const vec3& h = A.shape.half;
    static const float C[8][3] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
    };
    m.normal = -pn;
    for (int c = 0; c < 8 && m.npts < 4; ++c) {
        const vec3 corner = A.pos + (body_axis(R, 0) * (C[c][0] * h.x)) +
                            (body_axis(R, 1) * (C[c][1] * h.y)) +
                            (body_axis(R, 2) * (C[c][2] * h.z));
        const float dist = dot(pn, corner) + pd;
        if (dist < 0) {
            ContactPoint& p = m.pts[m.npts++];
            p.pos = corner - pn * dist;
            p.pen = -dist;
            p.feature = (u32)c;
        }
    }
}

// ---------------------------------------------------------------- box-box SAT
struct ClipVert { vec3 p; u32 id; };

// keep vertices with dot(p,n) <= offset (Sutherland-Hodgman)
static int clip_poly(const vec3& n, float offset, const ClipVert* in, int nin, ClipVert* out) {
    int nout = 0;
    for (int i = 0; i < nin; ++i) {
        const ClipVert& a = in[i];
        const ClipVert& b = in[(i + 1) % nin];
        const float da = dot(a.p, n) - offset;
        const float db = dot(b.p, n) - offset;
        if (da <= 0 && nout < 8) out[nout++] = a;
        if ((da <= 0) != (db <= 0) && nout < 8) {
            const float t = da / (da - db);
            ClipVert v;
            v.p = a.p + (b.p - a.p) * t;
            v.id = a.id;
            out[nout++] = v;
        }
    }
    return nout;
}

// Build a reference/incident face manifold. ref_normal points from ref box toward incident box.
static void face_manifold(const Body& ref, const Body& inc,
                          const vec3& ref_normal, float ref_face_half,
                          const vec3& inc_normal, float inc_face_half,
                          const vec3& axis_ab, Manifold& m) {
    float Rr[9], Ri[9];
    quat_to_mat3(ref.rot, Rr);
    quat_to_mat3(inc.rot, Ri);
    const vec3 rax[3] = { body_axis(Rr, 0), body_axis(Rr, 1), body_axis(Rr, 2) };
    const vec3 iax[3] = { body_axis(Ri, 0), body_axis(Ri, 1), body_axis(Ri, 2) };
    const vec3& rh = ref.shape.half;
    const vec3& ih = inc.shape.half;

    // index of ref axis along ref_normal
    int rni = 0; float bd = -1.0f;
    for (int i = 0; i < 3; ++i) { const float d = std::fabs(dot(rax[i], ref_normal)); if (d > bd) { bd = d; rni = i; } }
    int ini = 0; bd = -1.0f;
    for (int i = 0; i < 3; ++i) { const float d = std::fabs(dot(iax[i], inc_normal)); if (d > bd) { bd = d; ini = i; } }
    const int ru = (rni + 1) % 3, rv = (rni + 2) % 3;
    const int iu = (ini + 1) % 3, iv = (ini + 2) % 3;

    const vec3 ref_fc = ref.pos + ref_normal * comp(rh, rni);

    const vec3 inc_fc = inc.pos + inc_normal * comp(ih, ini);
    // incident face quad
    ClipVert poly[8];
    int npoly = 0;
    const vec3 iu_ax = iax[iu] * comp(ih, iu);
    const vec3 iv_ax = iax[iv] * comp(ih, iv);
    poly[npoly++] = { inc_fc - iu_ax - iv_ax, 0 };
    poly[npoly++] = { inc_fc + iu_ax - iv_ax, 1 };
    poly[npoly++] = { inc_fc + iu_ax + iv_ax, 2 };
    poly[npoly++] = { inc_fc - iu_ax + iv_ax, 3 };

    // clip against the 4 side planes of the reference face
    ClipVert tmp[8];
    const float ru_h = comp(rh, ru), rv_h = comp(rh, rv);
    npoly = clip_poly( rax[ru],  dot(rax[ru], ref_fc) + ru_h, poly, npoly, tmp); if (!npoly) return;
    npoly = clip_poly(-rax[ru], -dot(rax[ru], ref_fc) + ru_h, poly, npoly, tmp); if (!npoly) return;
    npoly = clip_poly( rax[rv],  dot(rax[rv], ref_fc) + rv_h, poly, npoly, tmp); if (!npoly) return;
    npoly = clip_poly(-rax[rv], -dot(rax[rv], ref_fc) + rv_h, poly, npoly, tmp); if (!npoly) return;
    std::memcpy(poly, tmp, sizeof(ClipVert) * (size_t)npoly);

    // keep points below the reference face surface
    struct Cand { float depth; vec3 pos; u32 id; };
    Cand cand[8];
    int ncand = 0;
    for (int i = 0; i < npoly; ++i) {
        const float depth = dot(poly[i].p - ref_fc, ref_normal); // <0 => penetrating
        if (depth <= 0.0f) cand[ncand++] = { depth, poly[i].p, poly[i].id };
    }
    if (!ncand) return;
    // deepest first, cap at 4
    std::sort(cand, cand + ncand, [](const Cand& a, const Cand& b) { return a.depth < b.depth; });
    if (ncand > 4) ncand = 4;
    m.normal = axis_ab;
    for (int i = 0; i < ncand; ++i) {
        ContactPoint& p = m.pts[m.npts++];
        p.pos = cand[i].pos - ref_normal * cand[i].depth; // project onto ref face
        p.pen = -cand[i].depth;
        p.feature = cand[i].id;
    }
    (void)ref_face_half; (void)inc_face_half;
}

static void collide_box_box(const Body& A, const Body& B, Manifold& m) {
    float RA[9], RB[9];
    quat_to_mat3(A.rot, RA);
    quat_to_mat3(B.rot, RB);
    const vec3 Aax[3] = { body_axis(RA, 0), body_axis(RA, 1), body_axis(RA, 2) };
    const vec3 Bax[3] = { body_axis(RB, 0), body_axis(RB, 1), body_axis(RB, 2) };
    const vec3& ha = A.shape.half;
    const vec3& hb = B.shape.half;
    const vec3 d = B.pos - A.pos;

    float best_sep = -1e30f;
    int kind = -1, bi = -1, bj = -1;
    vec3 axis(0, 1, 0); // A -> B

    // face axes of A
    for (int i = 0; i < 3; ++i) {
        const float s = dot(d, Aax[i]);
        const float rb = std::fabs(dot(Aax[i], Bax[0])) * hb.x +
                         std::fabs(dot(Aax[i], Bax[1])) * hb.y +
                         std::fabs(dot(Aax[i], Bax[2])) * hb.z;
        const float sep = std::fabs(s) - (comp(ha, i) + rb);
        if (sep > 0) return;
        if (sep > best_sep) { best_sep = sep; kind = 0; bi = i; axis = Aax[i] * (s >= 0 ? 1.0f : -1.0f); }
    }
    // face axes of B
    for (int j = 0; j < 3; ++j) {
        const float s = dot(d, Bax[j]);
        const float ra = std::fabs(dot(Aax[0], Bax[j])) * ha.x +
                         std::fabs(dot(Aax[1], Bax[j])) * ha.y +
                         std::fabs(dot(Aax[2], Bax[j])) * ha.z;
        const float sep = std::fabs(s) - (comp(hb, j) + ra);
        if (sep > 0) return;
        if (sep > best_sep) { best_sep = sep; kind = 1; bj = j; axis = Bax[j] * (s >= 0 ? 1.0f : -1.0f); }
    }
    // edge-edge axes
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) {
            vec3 ax = cross(Aax[i], Bax[j]);
            const float L = length(ax);
            if (L < 1e-6f) continue;
            ax = ax / L;
            const float s = dot(d, ax);
            float ra = std::fabs(dot(Aax[0], ax)) * ha.x +
                       std::fabs(dot(Aax[1], ax)) * ha.y +
                       std::fabs(dot(Aax[2], ax)) * ha.z;
            float rb = std::fabs(dot(Bax[0], ax)) * hb.x +
                       std::fabs(dot(Bax[1], ax)) * hb.y +
                       std::fabs(dot(Bax[2], ax)) * hb.z;
            const float sep = std::fabs(s) - (ra + rb);
            if (sep > 0) return;
            if (sep > best_sep) { best_sep = sep; kind = 2; bi = i; bj = j; axis = ax * (s >= 0 ? 1.0f : -1.0f); }
        }

    if (kind == 0) {
        // reference face on A
        face_manifold(A, B, axis, 0, axis, 0, axis, m);
    } else if (kind == 1) {
        // reference face on B; ref normal (B->A) = -axis
        face_manifold(B, A, -axis, 0, -axis, 0, axis, m);
    } else {
        // edge-edge: single contact at the closest points of the two edges
        const vec3 ea = Aax[bi], eb = Bax[bj];
        const vec3 r = A.pos - B.pos;
        const float a11 = dot(ea, ea), a12 = -dot(ea, eb), a22 = dot(eb, eb);
        const float b1 = -dot(ea, r), b2 = dot(eb, r);
        const float det = a11 * a22 - a12 * a12;
        float ta, tb;
        if (std::fabs(det) > 1e-12f) {
            ta = (a22 * b1 - a12 * b2) / det;
            tb = (a11 * b2 - a12 * b1) / det;
        } else { ta = 0; tb = 0; }
        const float hla = bi == 0 ? ha.x : bi == 1 ? ha.y : ha.z;
        const float hlb = bj == 0 ? hb.x : bj == 1 ? hb.y : hb.z;
        ta = clampf(ta, -hla, hla);
        tb = clampf(tb, -hlb, hlb);
        const vec3 pa = A.pos + ea * ta;
        const vec3 pb = B.pos + eb * tb;
        m.normal = axis;
        ContactPoint& p = m.pts[m.npts++];
        p.pos = (pa + pb) * 0.5f;
        p.pen = -best_sep;
        p.feature = 0x40u | (u32(bi) << 3) | u32(bj);
    }
}

// ---------------------------------------------------------------- dispatch
void collide_pair(const Body& A, const Body& B, Manifold& m);

void collide_pair(const Body& A, const Body& B, Manifold& m) {
    const ShapeType ta = A.shape.type, tb = B.shape.type;
    if (ta == ShapeType::Sphere && tb == ShapeType::Sphere) {
        collide_sphere_sphere(A, B, m);
    } else if (ta == ShapeType::Sphere && tb == ShapeType::Box) {
        collide_sphere_box(A, B, m);
    } else if (ta == ShapeType::Box && tb == ShapeType::Sphere) {
        collide_sphere_box(B, A, m);
        if (m.npts) m.normal = -m.normal;
    } else if (ta == ShapeType::Sphere && tb == ShapeType::Plane) {
        collide_sphere_plane(A, B, m);
    } else if (ta == ShapeType::Plane && tb == ShapeType::Sphere) {
        collide_sphere_plane(B, A, m);
        if (m.npts) m.normal = -m.normal;
    } else if (ta == ShapeType::Box && tb == ShapeType::Plane) {
        collide_box_plane(A, B, m);
    } else if (ta == ShapeType::Plane && tb == ShapeType::Box) {
        collide_box_plane(B, A, m);
        if (m.npts) m.normal = -m.normal;
    } else if (ta == ShapeType::Box && tb == ShapeType::Box) {
        collide_box_box(A, B, m);
    }
    if (m.npts) {
        m.friction = std::sqrt(A.mat.friction * B.mat.friction);
        m.restitution = maxf(A.mat.restitution, B.mat.restitution);
    }
}

} // namespace mtt::physics
