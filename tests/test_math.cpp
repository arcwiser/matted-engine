// Matted Engine - tests: math library (vec3, quat, mat4, aabb, plane, frustum, camera)
#include "vec_check.h"

#include "engine/core/math/math.h"

#include <chrono>

using namespace mtt;

static const float kEps = 1e-5f;

// ------------------------------------------------------------------ scalars
MTT_TEST(scalar_helpers) {
    MTT_EQ(clampf(5.0f, 0.0f, 1.0f), 1.0f);
    MTT_EQ(clampf(-5.0f, 0.0f, 1.0f), 0.0f);
    MTT_EQ(clampf(0.5f, 0.0f, 1.0f), 0.5f);
    MTT_EQ(lerpf(0.0f, 10.0f, 0.25f), 2.5f);
    MTT_EQ(lerpf(3.0f, 7.0f, 0.0f), 3.0f);
    MTT_EQ(lerpf(3.0f, 7.0f, 1.0f), 7.0f);
    MTT_EQ(saturatef(2.0f), 1.0f);
    MTT_EQ(saturatef(-1.0f), 0.0f);
    MTT_EQ(minf(3.0f, 2.0f), 2.0f);
    MTT_EQ(maxf(3.0f, 2.0f), 3.0f);
    MTT_NEAR(kDegToRad * 180.0f, kPi, kEps);
    MTT_NEAR(kRadToDeg * kPi, 180.0f, 1e-3f);
}

// ------------------------------------------------------------------ vec3
MTT_TEST(vec3_basic_ops) {
    const vec3 a(1, 2, 3), b(4, 5, 6);
    v3_near(a + b, vec3(5, 7, 9), kEps, "a+b", __FILE__, __LINE__);
    v3_near(b - a, vec3(3, 3, 3), kEps, "b-a", __FILE__, __LINE__);
    v3_near(-a, vec3(-1, -2, -3), kEps, "-a", __FILE__, __LINE__);
    v3_near(a * 2.0f, vec3(2, 4, 6), kEps, "a*2", __FILE__, __LINE__);
    v3_near(2.0f * a, vec3(2, 4, 6), kEps, "2*a", __FILE__, __LINE__);
    v3_near(a / 2.0f, vec3(0.5f, 1, 1.5f), kEps, "a/2", __FILE__, __LINE__);
    v3_near(a * b, vec3(4, 10, 18), kEps, "a*b componentwise", __FILE__, __LINE__);

    vec3 c = a;
    c += b; MTT_CHECK(c.x == 5 && c.y == 7 && c.z == 9, "c += b");
    c -= b; MTT_CHECK(c.x == 1 && c.y == 2 && c.z == 3, "c -= b");
    c *= 3; MTT_CHECK(c.x == 3 && c.y == 6 && c.z == 9, "c *= 3");

    MTT_EQ(dot(a, b), 32.0f);
    MTT_EQ(length_sq(vec3(3, 4, 0)), 25.0f);
    MTT_NEAR(length(vec3(3, 4, 0)), 5.0f, kEps);
    MTT_EQ(dist_sq(a, b), 27.0f);
}

MTT_TEST(vec3_normalize_and_friends) {
    v3_near(normalize(vec3(3, 0, 0)), vec3(1, 0, 0), kEps, "normalize(x*3)", __FILE__, __LINE__);
    v3_near(normalize(vec3(0, 0, 0)), vec3(0, 0, 0), kEps, "normalize(zero) is zero", __FILE__, __LINE__);
    const vec3 n = normalize(vec3(1, 1, 1));
    MTT_NEAR(length(n), 1.0f, kEps);

    v3_near(cross(vec3(1, 0, 0), vec3(0, 1, 0)), vec3(0, 0, 1), kEps, "cross(x,y)=z (right-handed)", __FILE__, __LINE__);
    v3_near(cross(vec3(0, 1, 0), vec3(0, 0, 1)), vec3(1, 0, 0), kEps, "cross(y,z)=x", __FILE__, __LINE__);
    MTT_NEAR(dot(cross(vec3(1, 2, 3), vec3(4, 5, 6)), vec3(1, 2, 3)), 0.0f, kEps);

    v3_near(lerp(vec3(0, 0, 0), vec3(10, 10, 10), 0.3f), vec3(3, 3, 3), kEps, "lerp", __FILE__, __LINE__);
    v3_near(abs(vec3(-1, 2, -3)), vec3(1, 2, 3), kEps, "abs", __FILE__, __LINE__);
    v3_near(minv(vec3(1, 5, 3), vec3(2, 4, 6)), vec3(1, 4, 3), kEps, "minv", __FILE__, __LINE__);
    v3_near(maxv(vec3(1, 5, 3), vec3(2, 4, 6)), vec3(2, 5, 6), kEps, "maxv", __FILE__, __LINE__);
    v3_near(reflect(vec3(1, -1, 0), vec3(0, 1, 0)), vec3(1, 1, 0), kEps, "reflect off floor", __FILE__, __LINE__);
    v3_near(reflect(vec3(0, -1, 0), normalize(vec3(1, 1, 0))), vec3(-1, 0, 0), kEps, "reflect 45deg wall", __FILE__, __LINE__);
}

// ------------------------------------------------------------------ quat
MTT_TEST(quat_axis_angle_and_inverse) {
    const quat q = quat::from_axis_angle(vec3(0, 0, 1), kPi * 0.5f); // 90 deg about +Z
    v3_near(q.rotate(vec3(1, 0, 0)), vec3(0, 1, 0), kEps, "rotZ90 * x = y", __FILE__, __LINE__);
    v3_near(q.rotate(vec3(0, 1, 0)), vec3(-1, 0, 0), kEps, "rotZ90 * y = -x", __FILE__, __LINE__);
    v3_near(q.rotate_inv(q.rotate(vec3(0.3f, -1.2f, 5.0f))), vec3(0.3f, -1.2f, 5.0f), 1e-4f,
            "rotate_inv undoes rotate", __FILE__, __LINE__);

    // axis must be normalized by the caller for these checks to hold
    const quat qy = quat::from_axis_angle(normalize(vec3(0, 1, 0)), kPi * 0.5f);
    v3_near(qy.rotate(vec3(1, 0, 0)), vec3(0, 0, -1), kEps, "rotY90 * x = -z", __FILE__, __LINE__);

    const quat qi = quat::identity();
    v3_near(qi.rotate(vec3(7, -2, 3)), vec3(7, -2, 3), kEps, "identity rotation", __FILE__, __LINE__);

    const quat qn = quat(1, 1, 1, 1).normalized();
    MTT_NEAR(qn.x * qn.x + qn.y * qn.y + qn.z * qn.z + qn.w * qn.w, 1.0f, kEps);
    const quat qz0 = quat(0, 0, 0, 0).normalized(); // degenerate falls back to identity
    MTT_CHECK(qz0.x == 0 && qz0.y == 0 && qz0.z == 0 && qz0.w == 1, "normalizing zero quat falls back to identity");
}

MTT_TEST(quat_composition) {
    const quat qz = quat::from_axis_angle(normalize(vec3(0, 0, 1)), kPi * 0.5f);
    const quat qy = quat::from_axis_angle(normalize(vec3(0, 1, 0)), kPi * 0.5f);
    // (a*b).rotate(v) must equal a.rotate(b.rotate(v))
    const quat qzy = qz * qy;
    v3_near(qzy.rotate(vec3(1, 0, 0)), qz.rotate(qy.rotate(vec3(1, 0, 0))), kEps,
            "quat mul composes rotations", __FILE__, __LINE__);
    // Rz90 * Ry90 * x = Rz90 * (0,0,-1) = (0,0,-1)
    v3_near(qzy.rotate(vec3(1, 0, 0)), vec3(0, 0, -1), kEps, "Rz90*Ry90*x = -z", __FILE__, __LINE__);

    // two 45 deg yaw rotations == one 90 deg
    const quat h = quat::from_axis_angle(vec3(0, 1, 0), kPi * 0.25f);
    v3_near((h * h).rotate(vec3(1, 0, 0)), vec3(0, 0, -1), kEps, "45+45 deg yaw = 90", __FILE__, __LINE__);

    // conjugate of product: (a*b)^-1 == b^-1 * a^-1
    const quat inv = (qz * qy).conjugate();
    v3_near(inv.rotate(qzy.rotate(vec3(0.4f, 2.0f, -3.0f))), vec3(0.4f, 2.0f, -3.0f), 1e-4f,
            "conjugate inverts product", __FILE__, __LINE__);
}

MTT_TEST(quat_to_mat3_matches_rotate) {
    const quat q = quat::from_axis_angle(normalize(vec3(1, 2, 3)), 1.1f);
    float m[9];
    quat_to_mat3(q, m);
    const vec3 v(0.7f, -1.3f, 2.2f);
    v3_near(mat3_mul(m, v), q.rotate(v), 1e-4f, "mat3_mul == quat.rotate", __FILE__, __LINE__);

    // orthonormal rotation matrix: columns unit length, mutually orthogonal
    const vec3 c0(m[0], m[1], m[2]), c1(m[3], m[4], m[5]), c2(m[6], m[7], m[8]);
    MTT_NEAR(length_sq(c0), 1.0f, 1e-4f);
    MTT_NEAR(length_sq(c1), 1.0f, 1e-4f);
    MTT_NEAR(length_sq(c2), 1.0f, 1e-4f);
    MTT_NEAR(dot(c0, c1), 0.0f, 1e-4f);
    MTT_NEAR(dot(c0, c2), 0.0f, 1e-4f);
    MTT_NEAR(dot(c1, c2), 0.0f, 1e-4f);
}

// ------------------------------------------------------------------ mat4
MTT_TEST(mat4_identity_and_mul) {
    const mat4 I = mat4::identity();
    MTT_CHECK(I.m[0] == 1 && I.m[5] == 1 && I.m[10] == 1 && I.m[15] == 1, "identity diagonal");
    MTT_CHECK(I.m[1] == 0 && I.m[4] == 0 && I.m[12] == 0 && I.m[14] == 0, "identity off-diagonal");

    const mat4 A = mat4::trs(vec3(1, 2, 3), quat::from_axis_angle(vec3(0, 1, 0), 0.7f), vec3(1, 1, 1));
    const mat4 AI = mat4::mul(A, I);
    const mat4 IA = mat4::mul(I, A);
    for (int i = 0; i < 16; ++i) {
        MTT_NEAR(AI.m[i], A.m[i], 1e-6f);
        MTT_NEAR(IA.m[i], A.m[i], 1e-6f);
    }
    // associativity spot check
    const mat4 B = mat4::trs(vec3(-3, 5, 2), quat::from_axis_angle(vec3(1, 0, 0), 1.3f), vec3(2, 1, 0.5f));
    const mat4 AB_c = mat4::mul(mat4::mul(A, B), I);
    const mat4 A_Bc = mat4::mul(A, mat4::mul(B, I));
    for (int i = 0; i < 16; ++i) MTT_NEAR(AB_c.m[i], A_Bc.m[i], 1e-4f);
}

MTT_TEST(mat4_trs_transform_point) {
    const mat4 t = mat4::trs(vec3(1, 2, 3), quat::identity(), vec3(1, 1, 1));
    v3_near(t.transform_point(vec3(0, 0, 0)), vec3(1, 2, 3), kEps, "translation only", __FILE__, __LINE__);
    v3_near(t.transform_point(vec3(1, 1, 1)), vec3(2, 3, 4), kEps, "translation only v", __FILE__, __LINE__);

    // scale + rotate + translate: R_y90 maps (1,0,0) -> (0,0,-1), then scale x by 2 -> (0,0,-2)
    const mat4 s = mat4::trs(vec3(10, 0, 0), quat::from_axis_angle(vec3(0, 1, 0), kPi * 0.5f), vec3(2, 3, 4));
    v3_near(s.transform_point(vec3(1, 0, 0)), vec3(10, 0, -2), kEps, "R*S applied to (1,0,0)", __FILE__, __LINE__);
    v3_near(s.transform_point(vec3(0, 1, 0)), vec3(10, 3, 0), kEps, "R*S applied to (0,1,0)", __FILE__, __LINE__);

    // transform_dir ignores translation
    v3_near(s.transform_dir(vec3(1, 0, 0)), vec3(0, 0, -2), kEps, "dir has no translation", __FILE__, __LINE__);
    // row() accessor returns row-major view of the column-major storage
    const mat4 I = mat4::identity();
    v3_near(I.row(0).xyz(), vec3(1, 0, 0), kEps, "row(0)", __FILE__, __LINE__);
    MTT_EQ(I.row(3).w, 1.0f);
}

MTT_TEST(mat4_perspective_ndc_mapping) {
    const float fov = 60.0f * kDegToRad, aspect = 16.0f / 9.0f, zn = 0.1f, zf = 500.0f;
    const mat4 p = mat4::perspective(fov, aspect, zn, zf);

    // points on the view axis: NDC z = 0 at the near plane, 1 at the far plane
    auto ndc_z = [&](float z) { // z negative in front of camera
        const float clip_z = p.m[10] * z + p.m[14];
        const float clip_w = -z;
        return clip_z / clip_w;
    };
    MTT_NEAR(ndc_z(-zn), 0.0f, 1e-4f);
    MTT_NEAR(ndc_z(-zf), 1.0f, 1e-4f);
    // perspective depth is hyperbolic: NDC z = 0.5 sits much closer to the eye
    // than the midpoint of [zn, zf] (z = -2*zn*zf/(zf-zn) here)
    MTT_NEAR(ndc_z(-2.0f * zn * zf / (zf - zn)), 0.5f, 1e-3f);

    // x at the right edge of the frustum maps to NDC x = 1
    const float half_h = std::tan(fov * 0.5f) * zn;
    const float clip_x = p.m[0] * (half_h * aspect) + p.m[12];
    MTT_NEAR(clip_x / zn, 1.0f, 1e-4f);

    // A vertex inside the frustum must project inside NDC (|x|,|y| <= 1)
    auto ndc = [&](const vec3& view) {
        const vec4 c = { p.m[0] * view.x + p.m[8] * view.z,
                         p.m[5] * view.y + p.m[9] * view.z,
                         p.m[10] * view.z + p.m[14],
                         -view.z };
        return vec3(c.x / c.w, c.y / c.w, c.z / c.w);
    };
    const vec3 in = ndc(vec3(0.5f, -0.5f, -5.0f));
    MTT_CHECK(std::fabs(in.x) <= 1.0f && std::fabs(in.y) <= 1.0f, "inside frustum projects inside NDC");
    const vec3 out = ndc(vec3(0.0f, 100.0f, -5.0f));
    MTT_CHECK(out.y > 1.0f, "above frustum projects outside NDC");
}

MTT_TEST(mat4_look_at_frames) {
    const mat4 v = mat4::look_at(vec3(0, 0, 5), vec3(0, 0, 0), vec3(0, 1, 0));
    v3_near(v.transform_point(vec3(0, 0, 0)), vec3(0, 0, -5), kEps, "target in front (-z)", __FILE__, __LINE__);
    v3_near(v.transform_point(vec3(0, 0, 5)), vec3(0, 0, 0), kEps, "eye at origin", __FILE__, __LINE__);
    v3_near(v.transform_point(vec3(1, 0, 5)), vec3(1, 0, 0), kEps, "right is +x", __FILE__, __LINE__);
    v3_near(v.transform_point(vec3(0, 1, 5)), vec3(0, 1, 0), kEps, "up is +y", __FILE__, __LINE__);

    // arbitrary placement: eye maps to origin, forward maps to -z
    const vec3 eye(3, -2, 8), at(-4, 1, 2);
    const mat4 v2 = mat4::look_at(eye, at, vec3(0, 1, 0));
    v3_near(v2.transform_point(eye), vec3(0, 0, 0), kEps, "eye -> origin", __FILE__, __LINE__);
    const vec3 fwd = normalize(at - eye);
    const float along = v2.transform_dir(fwd).z;
    MTT_NEAR(along, -1.0f, 1e-4f);
}

// ------------------------------------------------------------------ aabb
MTT_TEST(aabb_basic_ops) {
    aabb b = aabb::empty();
    MTT_CHECK(b.is_empty(), "fresh aabb is empty");
    MTT_CHECK(!b.intersects(aabb::empty()), "empty does not intersect empty");

    b.expand_point(vec3(-1, -2, -3));
    b.expand_point(vec3(2, 1, 4));
    MTT_CHECK(!b.is_empty(), "expanded aabb not empty");
    v3_near(b.center(), vec3(0.5f, -0.5f, 0.5f), kEps, "center", __FILE__, __LINE__);
    v3_near(b.extents(), vec3(1.5f, 1.5f, 3.5f), kEps, "extents", __FILE__, __LINE__);

    aabb c = aabb::empty();
    c.expand_point(vec3(10, 10, 10));
    c.expand_aabb(b);
    v3_near(c.mn, vec3(-1, -2, -3), kEps, "expand_aabb mn", __FILE__, __LINE__);
    v3_near(c.mx, vec3(10, 10, 10), kEps, "expand_aabb mx", __FILE__, __LINE__);

    MTT_CHECK(b.intersects(b), "self intersection");
    aabb touching = b;
    touching.mn = vec3(2, 1, 4); // shares the mx corner
    touching.mx = vec3(5, 5, 5);
    MTT_CHECK(b.intersects(touching), "touching boxes intersect (<=)");
    aabb apart = touching;
    apart.mn = vec3(2.01f, 1, 4);
    MTT_CHECK(!b.intersects(apart), "separated boxes do not intersect");
}

MTT_TEST(aabb_transformed) {
    aabb u = aabb::empty();
    u.expand_point(vec3(-1, -1, -1));
    u.expand_point(vec3(1, 1, 1));

    // translate + uniform scale, no rotation
    const aabb ts = u.transformed(vec3(5, 0, -2), quat::identity(), vec3(2, 2, 2));
    v3_near(ts.mn, vec3(3, -2, -4), kEps, "scaled+translated mn", __FILE__, __LINE__);
    v3_near(ts.mx, vec3(7, 2, 0), kEps, "scaled+translated mx", __FILE__, __LINE__);

    // 45 deg rotation about Z inflates x/y half-extents to sqrt(2)
    const aabb rot = u.transformed(vec3(0, 0, 0), quat::from_axis_angle(vec3(0, 0, 1), kPi * 0.25f), vec3(1, 1, 1));
    const float s2 = std::sqrt(2.0f);
    MTT_NEAR(rot.mx.x, s2, 1e-4f);
    MTT_NEAR(rot.mn.x, -s2, 1e-4f);
    MTT_NEAR(rot.mx.z, 1.0f, 1e-4f);

    // result must contain the transformed center
    const aabb r2 = u.transformed(vec3(9, 9, 9), quat::from_axis_angle(normalize(vec3(1, 2, 3)), 2.0f), vec3(1, 2, 3));
    const vec3 c = r2.center();
    MTT_CHECK(c.x >= r2.mn.x && c.x <= r2.mx.x &&
              c.y >= r2.mn.y && c.y <= r2.mx.y &&
              c.z >= r2.mn.z && c.z <= r2.mx.z, "center inside transformed box");
}

// ------------------------------------------------------------------ plane & frustum
MTT_TEST(plane_distance) {
    const plane floor{ vec3(0, 1, 0), 0.0f };
    MTT_NEAR(floor.distance(vec3(0, 5, 0)), 5.0f, kEps);
    MTT_NEAR(floor.distance(vec3(3, -2, 7)), -2.0f, kEps);
    MTT_NEAR(floor.distance(vec3(9, 0, 9)), 0.0f, kEps);

    const plane offset{ vec3(0, 1, 0), -3.0f }; // y = 3 plane
    MTT_NEAR(offset.distance(vec3(0, 3, 0)), 0.0f, kEps);
    MTT_NEAR(offset.distance(vec3(0, 8, 0)), 5.0f, kEps);
}

MTT_TEST(frustum_from_camera_culls_boxes) {
    Camera cam;
    cam.pos = vec3(0, 0, 0);
    cam.yaw = 0; cam.pitch = 0;
    cam.fov_y = 60.0f * kDegToRad;
    cam.near_z = 0.1f;
    cam.far_z = 500.0f;

    frustum fru;
    fru.build_from_viewproj(cam.view_proj(16.0f / 9.0f));

    // camera must be strictly inside its own frustum (all planes positive)
    for (int i = 0; i < 6; ++i)
        MTT_CHECK(fru.pl[i].distance(cam.pos) > 0.0f, "camera inside frustum plane i");

    auto box_at = [](const vec3& c, float half) {
        aabb b = aabb::empty();
        b.expand_point(c - vec3(half, half, half));
        b.expand_point(c + vec3(half, half, half));
        return b;
    };

    MTT_CHECK(fru.intersects(box_at(vec3(0, 0, -10), 1.0f)), "box in front visible");
    MTT_CHECK(fru.intersects(box_at(vec3(6, 2, -40), 1.0f)), "box off-center but inside visible");
    MTT_CHECK(!fru.intersects(box_at(vec3(0, 0, 10), 1.0f)), "box behind camera culled");
    MTT_CHECK(!fru.intersects(box_at(vec3(0, 0, -1000), 1.0f)), "box beyond far plane culled");
    MTT_CHECK(!fru.intersects(box_at(vec3(0, 50, -10), 1.0f)), "box above frustum culled");
    MTT_CHECK(!fru.intersects(box_at(vec3(0, -50, -10), 1.0f)), "box below frustum culled");
    MTT_CHECK(!fru.intersects(box_at(vec3(100, 0, -10), 1.0f)), "box far right culled");

    // conservative: a box that merely straddles a plane still intersects
    MTT_CHECK(fru.intersects(box_at(vec3(0, 0, 0), 2.0f)), "box straddling near plane visible");
    MTT_CHECK(fru.intersects(box_at(vec3(7, 0, -10), 3.0f)), "box straddling right plane visible");

    // a huge box enclosing the whole frustum must pass every plane test
    MTT_CHECK(fru.intersects(box_at(vec3(0, 0, -250), 600.0f)), "enclosing box visible");
}

// ------------------------------------------------------------------ camera basis
MTT_TEST(camera_basis_vectors) {
    Camera cam;
    cam.pos = vec3(0, 2, 6);
    cam.yaw = 0; cam.pitch = 0;
    v3_near(cam.forward(), vec3(0, 0, -1), kEps, "yaw0 forward is -z", __FILE__, __LINE__);
    v3_near(cam.right(), vec3(1, 0, 0), kEps, "yaw0 right is +x", __FILE__, __LINE__);
    v3_near(cam.up(), vec3(0, 1, 0), kEps, "yaw0 up is +y", __FILE__, __LINE__);

    cam.yaw = 90.0f * kDegToRad;
    v3_near(cam.forward(), vec3(-1, 0, 0), kEps, "yaw90 forward is -x", __FILE__, __LINE__);
    v3_near(cam.right(), vec3(0, 0, -1), kEps, "yaw90 right is -z", __FILE__, __LINE__);

    cam.yaw = 0; cam.pitch = 45.0f * kDegToRad;
    v3_near(cam.forward(), vec3(0, std::sin(kPi * 0.25f), -std::cos(kPi * 0.25f)), kEps,
            "pitch45 forward", __FILE__, __LINE__);

    // right is always horizontal, and the basis stays orthogonal
    cam.yaw = 2.1f; cam.pitch = 0.7f;
    const vec3 f = cam.forward(), r = cam.right(), u = cam.up();
    MTT_NEAR(dot(f, r), 0.0f, 1e-4f);
    MTT_NEAR(dot(f, u), 0.0f, 1e-4f);
    MTT_NEAR(dot(r, u), 0.0f, 1e-4f);
    MTT_NEAR(length(f), 1.0f, 1e-5f);
    MTT_NEAR(length(r), 1.0f, 1e-5f);

    // view space: forward maps to -z regardless of yaw/pitch
    const mat4 view = cam.view();
    v3_near(view.transform_dir(f), vec3(0, 0, -1), 1e-4f, "view maps forward to -z", __FILE__, __LINE__);
    v3_near(view.transform_point(cam.pos), vec3(0, 0, 0), 1e-4f, "view maps eye to origin", __FILE__, __LINE__);

    const mat4 vp = cam.view_proj(1.7777f);
    MTT_EQ(vp.m[11], -1.0f); // standard perspective sign
    for (int i = 0; i < 16; ++i) MTT_CHECK(std::isfinite(vp.m[i]), "view_proj finite");
}
