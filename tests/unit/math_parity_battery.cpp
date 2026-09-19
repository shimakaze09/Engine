// Prints a fixed battery of math results in exact hexadecimal float form so
// two builds of this file — one taking the SSE2 paths, one compiled with
// ENGINE_MATH_SSE2=0 — can be compared byte for byte by
// math_parity_test.cmake. Every function with a SIMD branch is exercised
// directly, and so are the callers that compound them (matrix product,
// slerp, axis-angle extraction, Vec4 length and normalize). A differing
// line is a place where the two implementations disagree in the last bit.

#include "engine/math/aabb.h"
#include "engine/math/mat4.h"
#include "engine/math/quat.h"
#include "engine/math/ray.h"
#include "engine/math/sphere.h"
#include "engine/math/transform.h"
#include "engine/math/vec3.h"
#include "engine/math/vec4.h"

#include <cstdint>
#include <cstdio>

namespace {

using engine::math::AABB;
using engine::math::Mat4;
using engine::math::Quat;
using engine::math::Ray;
using engine::math::Sphere;
using engine::math::Vec3;
using engine::math::Vec4;

constexpr int kIterations = 1024;

/// Fixed-seed generator whose float arithmetic is plain scalar, so both
/// builds draw byte-identical inputs.
struct Lcg final {
  std::uint32_t state = 0x9E3779B9U;

  float next(float lo, float hi) noexcept {
    state = (state * 1664525U) + 1013904223U;
    const float unit =
        static_cast<float>(state >> 8U) * (1.0F / 16777216.0F);
    return lo + ((hi - lo) * unit);
  }

  Vec3 vec3(float lo, float hi) noexcept {
    return Vec3(next(lo, hi), next(lo, hi), next(lo, hi));
  }

  Vec4 vec4(float lo, float hi) noexcept {
    return Vec4(next(lo, hi), next(lo, hi), next(lo, hi), next(lo, hi));
  }

  Quat quat(float lo, float hi) noexcept {
    return Quat{next(lo, hi), next(lo, hi), next(lo, hi), next(lo, hi)};
  }

  Mat4 mat4(float lo, float hi) noexcept {
    Mat4 out{};
    for (Vec4 &column : out.columns) {
      column = vec4(lo, hi);
    }
    return out;
  }
};

void print_f(const char *label, int i, float value) noexcept {
  std::printf("%s[%d] %a\n", label, i, static_cast<double>(value));
}

void print_v3(const char *label, int i, const Vec3 &v) noexcept {
  std::printf("%s[%d] %a %a %a\n", label, i, static_cast<double>(v.x),
              static_cast<double>(v.y), static_cast<double>(v.z));
}

void print_v4(const char *label, int i, const Vec4 &v) noexcept {
  std::printf("%s[%d] %a %a %a %a\n", label, i, static_cast<double>(v.x),
              static_cast<double>(v.y), static_cast<double>(v.z),
              static_cast<double>(v.w));
}

void print_q(const char *label, int i, const Quat &q) noexcept {
  std::printf("%s[%d] %a %a %a %a\n", label, i, static_cast<double>(q.x),
              static_cast<double>(q.y), static_cast<double>(q.z),
              static_cast<double>(q.w));
}

void print_m4(const char *label, int i, const Mat4 &m) noexcept {
  std::printf("%s[%d]", label, i);
  for (const Vec4 &column : m.columns) {
    std::printf(" %a %a %a %a", static_cast<double>(column.x),
                static_cast<double>(column.y), static_cast<double>(column.z),
                static_cast<double>(column.w));
  }
  std::printf("\n");
}

} // namespace

/// Runs this executable or test program.
int main() {
  // The compare script refuses to pass unless one side reports 1 and the
  // other 0; on a host without SSE2 both would be scalar and the test is
  // reported skipped rather than vacuously green.
  std::printf("sse2=%d\n", ENGINE_MATH_SSE2);

  Lcg rng{};
  for (int i = 0; i < kIterations; ++i) {
    const Vec4 a = rng.vec4(-100.0F, 100.0F);
    const Vec4 b = rng.vec4(-100.0F, 100.0F);
    const float s = rng.next(-10.0F, 10.0F);
    const float sNonZero = (s < 0.0F) ? (s - 0.5F) : (s + 0.5F);
    const float t = rng.next(0.0F, 1.0F);
    print_v4("v4_add", i, engine::math::add(a, b));
    print_v4("v4_sub", i, engine::math::sub(a, b));
    print_v4("v4_mul", i, engine::math::mul(a, s));
    print_v4("v4_div", i, engine::math::div(a, sNonZero));
    print_f("v4_dot", i, engine::math::dot(a, b));
    print_f("v4_length", i, engine::math::length(a));
    print_v4("v4_normalize", i, engine::math::normalize(a));
    print_v4("v4_lerp", i, engine::math::lerp(a, b, t));

    const Mat4 m = rng.mat4(-10.0F, 10.0F);
    const Mat4 n = rng.mat4(-10.0F, 10.0F);
    print_v4("m4_mul_v4", i, engine::math::mul(m, a));
    print_m4("m4_mul_m4", i, engine::math::mul(m, n));
    print_m4("m4_transpose", i, engine::math::transpose(m));

    const Quat p = rng.quat(-1.0F, 1.0F);
    const Quat q = rng.quat(-1.0F, 1.0F);
    print_q("q_conjugate", i, engine::math::conjugate(p));
    print_f("q_dot", i, engine::math::dot(p, q));
    print_q("q_normalize", i, engine::math::normalize(p));
    print_q("q_slerp", i,
            engine::math::slerp(engine::math::normalize(p),
                                engine::math::normalize(q), t));
    Vec3 axis{};
    float radians = 0.0F;
    if (engine::math::to_axis_angle(p, &axis, &radians)) {
      print_v3("q_axis", i, axis);
      print_f("q_angle", i, radians);
    } else {
      std::printf("q_axis_angle[%d] rejected\n", i);
    }
    const Vec3 unitAxis = engine::math::normalize(rng.vec3(-1.0F, 1.0F));
    print_q("q_from_axis_angle", i,
            engine::math::from_axis_angle(unitAxis, rng.next(-6.0F, 6.0F)));

    // Projections and bounds have no SIMD branch of their own but sit on
    // the same Vec4/Mat4 storage, so they ride along at zero cost.
    const float nearZ = rng.next(0.05F, 1.0F);
    const float farZ = nearZ + rng.next(1.0F, 1000.0F);
    print_m4("perspective", i,
             engine::math::perspective(rng.next(0.2F, 2.8F),
                                       rng.next(0.5F, 2.0F), nearZ, farZ));
    const float left = rng.next(-50.0F, 0.0F);
    const float bottom = rng.next(-50.0F, 0.0F);
    print_m4("ortho_zero_one", i,
             engine::math::ortho_zero_one(left, left + rng.next(1.0F, 50.0F),
                                          bottom, bottom + rng.next(1.0F, 50.0F),
                                          nearZ, farZ));
    print_v3("aabb_half_extents", i,
             engine::math::transform_aabb_half_extents(
                 m, rng.vec3(0.0F, 5.0F)));

    Ray ray{};
    ray.origin = rng.vec3(-5.0F, 5.0F);
    ray.direction = engine::math::normalize(rng.vec3(-1.0F, 1.0F));
    AABB box{};
    box.min = rng.vec3(-3.0F, 0.0F);
    box.max = rng.vec3(0.0F, 3.0F);
    float hitT = 0.0F;
    if (engine::math::ray_intersects_aabb(ray, box, &hitT)) {
      print_f("ray_aabb", i, hitT);
    } else {
      std::printf("ray_aabb[%d] miss\n", i);
    }
    Sphere sphere{};
    sphere.center = rng.vec3(-2.0F, 2.0F);
    sphere.radius = rng.next(0.5F, 3.0F);
    if (engine::math::ray_intersects_sphere(ray, sphere, &hitT)) {
      print_f("ray_sphere", i, hitT);
    } else {
      std::printf("ray_sphere[%d] miss\n", i);
    }
  }
  return 0;
}
