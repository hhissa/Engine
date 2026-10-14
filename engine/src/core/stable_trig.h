#pragma once
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Euler <-> quaternion conversions that give the SAME BITS on every
// platform, for the places where a rotation feeds the chunk cache key.
//
// The key fingerprints the bytes each primitive uploads (see
// VulkanRaymarchShader's primitive_content_hash_), and a primitive's
// rotation is uploaded as a quaternion built with cos/sin -- plus, under a
// layer transform, a position and Euler triple that went through
// atan2/asin (sdf_layer_world_transform()). Those come from the C runtime,
// and glibc and Windows' UCRT are free to round them differently in the
// last bit. One differing bit is a different key, so a chunk cache baked
// on Linux and shipped with the Windows build (assets/chunk_cache, see
// tools/bake-chunk-cache.sh) would quietly miss for every chunk a rotated
// primitive reaches. Never wrong -- a miss just bakes -- but it throws the
// shipped cache away.
//
// So these never call the C runtime. Each function works in double with
// only +, -, *, / and sqrt, which IEEE 754 requires to be correctly rounded
// everywhere, and rounds to float once at the end. Accurate to well under a
// float ulp, so what renders is unchanged; what changes is that it is the
// same on every machine.
//
// Holds only while the compiler doesn't fuse a*b+c into an FMA, which
// rounds once instead of twice: STABLE_TRIG_NO_FMA forbids it in each
// function body (scoped to that body -- at namespace scope the pragma
// would stay in force for the rest of whatever file includes this), even
// under -march=native or -ffp-contract=fast. Every build of this project
// uses clang; another compiler must be checked for the same guarantee.
#if defined(__clang__)
#define STABLE_TRIG_NO_FMA _Pragma("clang fp contract(off)")
#else
#define STABLE_TRIG_NO_FMA
#endif

namespace stable_trig {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kHalfPi = 1.57079632679489661923;

// sin and cos of x, for |x| up to a few thousand radians (Euler angles
// are a few radians at most).
inline void sincos(double x, double &s, double &c) {
  STABLE_TRIG_NO_FMA
  // Cody-Waite reduction by pi/2: x = k*pi/2 + r, |r| <= pi/4, with
  // fdlibm's split of pi/2 (two 33-bit parts and the remainder), so k*part
  // is exact for the first two.
  constexpr double kTwoOverPi = 0.63661977236758134308;
  constexpr double kPio2Hi = 1.57079632673412561417e+00;
  constexpr double kPio2Mid = 6.07710050630396597660e-11;
  constexpr double kPio2Lo = 2.02226624879595063154e-21;
  double kf = x * kTwoOverPi;
  kf = kf >= 0.0 ? static_cast<double>(static_cast<long long>(kf + 0.5))
                 : -static_cast<double>(static_cast<long long>(-kf + 0.5));
  const double r = ((x - kf * kPio2Hi) - kf * kPio2Mid) - kf * kPio2Lo;
  const double r2 = r * r;
  // Taylor series to the 17th / 16th power -- the next term is below
  // 1e-19 for |r| <= pi/4.
  double ps = 1.0 / 355687428096000.0;           // 1/17!
  ps = ps * r2 - 1.0 / 1307674368000.0;           // 1/15!
  ps = ps * r2 + 1.0 / 6227020800.0;              // 1/13!
  ps = ps * r2 - 1.0 / 39916800.0;                // 1/11!
  ps = ps * r2 + 1.0 / 362880.0;                  // 1/9!
  ps = ps * r2 - 1.0 / 5040.0;                    // 1/7!
  ps = ps * r2 + 1.0 / 120.0;                     // 1/5!
  ps = ps * r2 - 1.0 / 6.0;                       // 1/3!
  const double sin_r = r + r * r2 * ps;
  double pc = 1.0 / 20922789888000.0;             // 1/16!
  pc = pc * r2 - 1.0 / 87178291200.0;             // 1/14!
  pc = pc * r2 + 1.0 / 479001600.0;               // 1/12!
  pc = pc * r2 - 1.0 / 3628800.0;                 // 1/10!
  pc = pc * r2 + 1.0 / 40320.0;                   // 1/8!
  pc = pc * r2 - 1.0 / 720.0;                     // 1/6!
  pc = pc * r2 + 1.0 / 24.0;                      // 1/4!
  pc = pc * r2 - 0.5;                             // 1/2!
  const double cos_r = 1.0 + r2 * pc;
  switch (static_cast<long long>(kf) & 3) {
  case 0: s = sin_r; c = cos_r; break;
  case 1: s = cos_r; c = -sin_r; break;
  case 2: s = -sin_r; c = -cos_r; break;
  default: s = -cos_r; c = sin_r; break;
  }
}

// atan(t) for t >= 0.
inline double atan_nonnegative(double t) {
  STABLE_TRIG_NO_FMA
  // atan(1/t) = pi/2 - atan(t) brings t into [0, 1]; two halvings,
  // atan(t) = 2 atan(t / (1 + sqrt(1 + t^2))), bring it under
  // tan(pi/16) ~ 0.2, where the series' 13th term is below 1e-19.
  const bool inverted = t > 1.0;
  if (inverted) {
    t = 1.0 / t;
  }
  t = t / (1.0 + std::sqrt(1.0 + t * t));
  t = t / (1.0 + std::sqrt(1.0 + t * t));
  const double t2 = t * t;
  double p = 1.0 / 25.0;
  for (int n = 11; n >= 0; --n) {
    p = p * -t2 + 1.0 / (2.0 * n + 1.0);
  }
  const double a = 4.0 * (t * p);
  return inverted ? kHalfPi - a : a;
}

inline double atan2(double y, double x) {
  STABLE_TRIG_NO_FMA
  if (x == 0.0 && y == 0.0) {
    return 0.0;
  }
  if (x == 0.0) {
    return y > 0.0 ? kHalfPi : -kHalfPi;
  }
  const double a = atan_nonnegative(std::abs(y) / std::abs(x));
  const double angle = x > 0.0 ? a : kPi - a;
  return y < 0.0 ? -angle : angle;
}

inline double asin(double x) {
  STABLE_TRIG_NO_FMA
  x = x < -1.0 ? -1.0 : (x > 1.0 ? 1.0 : x);
  return atan2(x, std::sqrt((1.0 - x) * (1.0 + x)));
}

// glm::quat(euler), by the same formula (glm/detail/type_quat.inl).
inline glm::quat quat_from_euler(glm::vec3 euler) {
  STABLE_TRIG_NO_FMA
  double sx, cx, sy, cy, sz, cz;
  sincos(0.5 * static_cast<double>(euler.x), sx, cx);
  sincos(0.5 * static_cast<double>(euler.y), sy, cy);
  sincos(0.5 * static_cast<double>(euler.z), sz, cz);
  return glm::quat(static_cast<float>(cx * cy * cz + sx * sy * sz),
                   static_cast<float>(sx * cy * cz - cx * sy * sz),
                   static_cast<float>(cx * sy * cz + sx * cy * sz),
                   static_cast<float>(cx * cy * sz - sx * sy * cz));
}

// glm::eulerAngles(q), by the same formulas and the same gimbal-lock
// special cases (glm/gtc/quaternion.inl: pitch(), yaw(), roll()), whose
// threshold is epsilon<float>().
inline glm::vec3 euler_from_quat(glm::quat q) {
  STABLE_TRIG_NO_FMA
  constexpr double kEpsilon = 1.1920928955078125e-07; // FLT_EPSILON
  const double w = q.w, x = q.x, y = q.y, z = q.z;
  // pitch
  double py = 2.0 * (y * z + w * x);
  double px = w * w - x * x - y * y + z * z;
  double pitch = (std::abs(px) < kEpsilon && std::abs(py) < kEpsilon)
                     ? 2.0 * atan2(x, w)
                     : atan2(py, px);
  // yaw
  double yaw = asin(-2.0 * (x * z - w * y));
  // roll
  double ry = 2.0 * (x * y + w * z);
  double rx = w * w + x * x - y * y - z * z;
  double roll = (std::abs(rx) < kEpsilon && std::abs(ry) < kEpsilon) ? 0.0
                                                              : atan2(ry, rx);
  return glm::vec3(static_cast<float>(pitch), static_cast<float>(yaw),
                   static_cast<float>(roll));
}

} // namespace stable_trig
