#pragma once
#include <resources/expression.h>
#include <resources/sdf_scene.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

// Pure CPU-side ray-vs-primitive picking against an SdfScene already held
// in memory (SdfEditorWindow::scene_) -- no GPU readback needed.
//
// Rather than deriving an exact analytic ray intersection formula for each
// of SdfPrimitiveType's 15 shapes (some, like Torus/Octahedron/Pyramid, are
// awkward or ugly to invert exactly), this mirrors the same signed distance
// functions Builtin.RaymarchVoxelize.comp.glsl's primitive_sdf() evaluates
// (see sdf:: below -- kept in exact correspondence with that file, in the
// same per-type local-space convention) and sphere-traces against them,
// the same way the GPU's own render pass finds a hit (see raymarch() in
// Builtin.RaymarchShader.comp.glsl) -- adding a primitive type only ever
// needs its distance function written once, here and in the voxelize
// shader, instead of that plus a bespoke exact intersection test.
struct SceneRayHit {
  f32 distance;    // along the ray, world units -- for picking the nearest hit
  int layer_index; // which SdfScene::layers[] entry was hit (this editor
                   // puts exactly one primitive per layer -- see
                   // SdfEditorWindow::on_add_clicked())
};

namespace sdf {

inline f32 sphere_sdf(glm::vec3 p, f32 radius) {
  return glm::length(p) - radius;
}

inline f32 box_sdf(glm::vec3 p, glm::vec3 half_extents) {
  glm::vec3 q = glm::abs(p) - half_extents;
  return glm::length(glm::max(q, glm::vec3(0.0f))) +
        std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f);
}

inline f32 plane_sdf(glm::vec3 p, f32 height) { return p.y - height; }

inline f32 torus_sdf(glm::vec3 p, f32 major_radius, f32 minor_radius) {
  glm::vec2 q(glm::length(glm::vec2(p.x, p.z)) - major_radius, p.y);
  return glm::length(q) - minor_radius;
}

inline f32 capped_cylinder_sdf(glm::vec3 p, f32 radius, f32 half_height) {
  glm::vec2 d = glm::abs(glm::vec2(glm::length(glm::vec2(p.x, p.z)), p.y)) -
               glm::vec2(radius, half_height);
  return std::min(std::max(d.x, d.y), 0.0f) +
        glm::length(glm::max(d, glm::vec2(0.0f)));
}

inline f32 capped_cone_sdf(glm::vec3 p, f32 half_height, f32 r1, f32 r2) {
  glm::vec2 q(glm::length(glm::vec2(p.x, p.z)), p.y);
  glm::vec2 k1(r2, half_height);
  glm::vec2 k2(r2 - r1, 2.0f * half_height);
  glm::vec2 ca(q.x - std::min(q.x, (q.y < 0.0f) ? r1 : r2),
              std::fabs(q.y) - half_height);
  glm::vec2 cb =
      q - k1 + k2 * std::clamp(glm::dot(k1 - q, k2) / glm::dot(k2, k2), 0.0f, 1.0f);
  f32 s = (cb.x < 0.0f && ca.y < 0.0f) ? -1.0f : 1.0f;
  return s * std::sqrt(std::min(glm::dot(ca, ca), glm::dot(cb, cb)));
}

inline f32 round_box_sdf(glm::vec3 p, glm::vec3 half_extents, f32 corner_radius) {
  glm::vec3 q = glm::abs(p) - half_extents + corner_radius;
  return glm::length(glm::max(q, glm::vec3(0.0f))) +
        std::min(std::max(q.x, std::max(q.y, q.z)), 0.0f) - corner_radius;
}

inline f32 box_frame_sdf(glm::vec3 p, glm::vec3 half_extents, f32 edge_thickness) {
  glm::vec3 pp = glm::abs(p) - half_extents;
  glm::vec3 q = glm::abs(pp + edge_thickness) - edge_thickness;
  f32 a = glm::length(glm::max(glm::vec3(pp.x, q.y, q.z), glm::vec3(0.0f))) +
         std::min(std::max(pp.x, std::max(q.y, q.z)), 0.0f);
  f32 b = glm::length(glm::max(glm::vec3(q.x, pp.y, q.z), glm::vec3(0.0f))) +
         std::min(std::max(q.x, std::max(pp.y, q.z)), 0.0f);
  f32 c = glm::length(glm::max(glm::vec3(q.x, q.y, pp.z), glm::vec3(0.0f))) +
         std::min(std::max(q.x, std::max(q.y, pp.z)), 0.0f);
  return std::min(std::min(a, b), c);
}

inline f32 octahedron_sdf(glm::vec3 p, f32 s) {
  glm::vec3 q = glm::abs(p);
  f32 m = q.x + q.y + q.z - s;
  glm::vec3 r;
  if (3.0f * q.x < m) {
    r = q;
  } else if (3.0f * q.y < m) {
    r = glm::vec3(q.y, q.z, q.x);
  } else if (3.0f * q.z < m) {
    r = glm::vec3(q.z, q.x, q.y);
  } else {
    return m * 0.57735027f;
  }
  f32 k = std::clamp(0.5f * (r.z - r.y + s), 0.0f, s);
  return glm::length(glm::vec3(r.x, r.y - s + k, r.z - k));
}

inline f32 pyramid_sdf(glm::vec3 p, f32 h) {
  f32 m2 = h * h + 0.25f;
  p.x = std::fabs(p.x);
  p.z = std::fabs(p.z);
  if (p.z > p.x) {
    std::swap(p.x, p.z);
  }
  p.x -= 0.5f;
  p.z -= 0.5f;
  glm::vec3 q(p.z, h * p.y - 0.5f * p.x, h * p.x + 0.5f * p.y);
  f32 s = std::max(-q.x, 0.0f);
  f32 t = std::clamp((q.y - 0.5f * p.z) / (m2 + 0.25f), 0.0f, 1.0f);
  f32 a = m2 * (q.x + s) * (q.x + s) + q.y * q.y;
  f32 b = m2 * (q.x + 0.5f * t) * (q.x + 0.5f * t) + (q.y - m2 * t) * (q.y - m2 * t);
  f32 d2 = (std::min(q.y, -q.x * m2 - q.y * 0.5f) > 0.0f) ? 0.0f : std::min(a, b);
  return std::sqrt((d2 + q.z * q.z) / m2) * (std::max(q.z, -p.y) < 0.0f ? -1.0f : 1.0f);
}

inline f32 hex_prism_sdf(glm::vec3 p, f32 inradius, f32 half_height) {
  const glm::vec3 k(-0.8660254f, 0.5f, 0.57735f);
  glm::vec3 q = glm::abs(p);
  glm::vec2 qxz(q.x, q.z);
  qxz -= 2.0f * std::min(glm::dot(glm::vec2(k.x, k.y), qxz), 0.0f) * glm::vec2(k.x, k.y);
  glm::vec2 d(
      glm::length(qxz - glm::vec2(std::clamp(qxz.x, -k.z * inradius, k.z * inradius),
                                 inradius)) *
          (qxz.y - inradius < 0.0f ? -1.0f : 1.0f),
      q.y - half_height);
  return std::min(std::max(d.x, d.y), 0.0f) + glm::length(glm::max(d, glm::vec2(0.0f)));
}

inline f32 round_cone_sdf(glm::vec3 p, f32 r1, f32 r2, f32 half_height) {
  p.y += half_height;
  f32 height = 2.0f * half_height;
  f32 b = (r1 - r2) / height;
  f32 a = std::sqrt(std::max(1.0f - b * b, 0.0f));
  glm::vec2 q(glm::length(glm::vec2(p.x, p.z)), p.y);
  f32 k = glm::dot(q, glm::vec2(-b, a));
  if (k < 0.0f) {
    return glm::length(q) - r1;
  }
  if (k > a * height) {
    return glm::length(q - glm::vec2(0.0f, height)) - r2;
  }
  return glm::dot(q, glm::vec2(a, b)) - r1;
}

inline f32 capsule_sdf(glm::vec3 p, f32 radius, f32 half_height) {
  p.y -= std::clamp(p.y, -half_height, half_height);
  return glm::length(p) - radius;
}

inline f32 link_sdf(glm::vec3 p, f32 half_length, f32 r1, f32 r2) {
  glm::vec3 q(p.x, std::max(std::fabs(p.y) - half_length, 0.0f), p.z);
  return glm::length(glm::vec2(glm::length(glm::vec2(q.x, q.y)) - r1, q.z)) - r2;
}

// Bound, not exact -- mirrors ellipsoid_sdf() in
// Builtin.RaymarchVoxelize.comp.glsl exactly (see its comment for why a
// bound is fine here too: raymarch_primitive() below safely under-steps
// rather than overshoots when d is merely a bound).
inline f32 ellipsoid_sdf(glm::vec3 p, glm::vec3 radii) {
  f32 k0 = glm::length(p / radii);
  f32 k1 = glm::length(p / (radii * radii));
  return k0 * (k0 - 1.0f) / k1;
}

// Dispatches to the primitive-specific function above by type, mirroring
// Builtin.RaymarchVoxelize.comp.glsl's primitive_sdf() exactly (minus the
// world-to-local transform, which raymarch_primitive() below handles).
// `params` is the primitive's *resolved* params.xyz/extra_param (see
// resolve_params() below) -- already has any parametric-attribute formula
// evaluated at this exact sample point, so this function itself never
// needs to know whether a given slot came from a constant or a formula.
inline f32 evaluate(SdfPrimitiveType type, glm::vec4 params, glm::vec3 local_p) {
  switch (type) {
  case SdfPrimitiveType::Sphere:
    return sphere_sdf(local_p, params.x);
  case SdfPrimitiveType::Box:
    return box_sdf(local_p, glm::vec3(params));
  case SdfPrimitiveType::Plane:
    return plane_sdf(local_p, params.x);
  case SdfPrimitiveType::Torus:
    return torus_sdf(local_p, params.x, params.y);
  case SdfPrimitiveType::CappedCylinder:
    return capped_cylinder_sdf(local_p, params.x, params.y);
  case SdfPrimitiveType::CappedCone:
    return capped_cone_sdf(local_p, params.x, params.y, params.z);
  case SdfPrimitiveType::RoundBox:
    return round_box_sdf(local_p, glm::vec3(params), params.w);
  case SdfPrimitiveType::BoxFrame:
    return box_frame_sdf(local_p, glm::vec3(params), params.w);
  case SdfPrimitiveType::Octahedron:
    return octahedron_sdf(local_p, params.x);
  case SdfPrimitiveType::Pyramid:
    return pyramid_sdf(local_p, params.x);
  case SdfPrimitiveType::HexPrism:
    return hex_prism_sdf(local_p, params.x, params.y);
  case SdfPrimitiveType::RoundCone:
    return round_cone_sdf(local_p, params.x, params.y, params.z);
  case SdfPrimitiveType::Capsule:
    return capsule_sdf(local_p, params.x, params.y);
  case SdfPrimitiveType::Link:
    return link_sdf(local_p, params.x, params.y, params.z);
  case SdfPrimitiveType::Ellipsoid:
    return ellipsoid_sdf(local_p, glm::vec3(params));
  }
  return std::numeric_limits<f32>::max();
}

} // namespace sdf

// A primitive's up-to-4 parametric-attribute formulas (params.x/y/z/
// extra_param, in that order -- see SdfPrimitiveDef::param_expressions),
// compiled once per raymarch_primitive() call rather than re-parsed at
// every sample point along the ray. nullopt in a slot means "no formula
// there, use the plain constant" (empty source or a compile failure --
// compile_expression() already logs why).
// Note: not named "slots" -- Qt's moc #defines that as a keyword-like
// macro (for "private slots:" etc.) in any translation unit that includes
// Qt headers, which silently mangles a member/variable of that name.
struct CompiledParamExprs {
  std::array<std::optional<CompiledExpression>, 4> entries;
};

inline CompiledParamExprs compile_param_expressions(const SdfPrimitiveDef &primitive) {
  CompiledParamExprs result;
  for (size_t i = 0; i < primitive.param_expressions.size(); ++i) {
    const std::string &source = primitive.param_expressions[i];
    if (!source.empty()) {
      result.entries[i] = compile_expression(source);
    }
  }
  return result;
}

// Resolves primitive's effective params at local-space point p: starts from
// its plain params.xyz/extra_param constants, then overrides whichever
// slots compiled has a compiled formula for, evaluated fresh at p -- mirrors
// resolve_params() in Builtin.RaymarchVoxelize.comp.glsl exactly, so a
// tapered/twisted shape picks the same way it renders.
inline glm::vec4 resolve_params(const SdfPrimitiveDef &primitive,
                                const CompiledParamExprs &compiled, glm::vec3 p) {
  glm::vec4 params(primitive.params, primitive.extra_param);
  for (size_t i = 0; i < compiled.entries.size(); ++i) {
    if (compiled.entries[i]) {
      params[static_cast<glm::vec4::length_type>(i)] =
          evaluate_expression(*compiled.entries[i], p);
    }
  }
  return params;
}

// Sphere-traces along origin+dir*t against primitive's distance function,
// in its own local space (world position subtracted, then rotated by the
// inverse of primitive.rotation -- skipped for Plane, which never rotates,
// matching primitive_sdf()'s engine-side convention exactly). Returns the
// hit distance t (>= 0), or nullopt if the ray never gets within epsilon of
// the surface before kMaxDist. Steps by abs(distance) each iteration (not
// the signed value) so progress is still made even if the ray starts
// inside the shape, the same safe-stepping convention
// Builtin.RaymarchShader.comp.glsl's raymarch() already uses.
inline std::optional<f32> raymarch_primitive(const SdfPrimitiveDef &primitive,
                                             glm::vec3 origin, glm::vec3 dir) {
  constexpr int kMaxSteps = 128;
  constexpr f32 kMaxDist = 100.0f;
  constexpr f32 kSurfaceEpsilon = 0.0005f;

  glm::vec3 local_origin = origin - primitive.position;
  glm::vec3 local_dir = dir;
  if (primitive.type != SdfPrimitiveType::Plane) {
    glm::quat inverse_rotation = glm::conjugate(glm::quat(primitive.rotation));
    local_origin = inverse_rotation * local_origin;
    local_dir = inverse_rotation * local_dir;
  }

  CompiledParamExprs compiled = compile_param_expressions(primitive);

  f32 t = 0.0f;
  for (int i = 0; i < kMaxSteps; ++i) {
    glm::vec3 p = local_origin + local_dir * t;
    glm::vec4 params = resolve_params(primitive, compiled, p);
    f32 d = sdf::evaluate(primitive.type, params, p);
    if (std::fabs(d) < kSurfaceEpsilon) {
      return t;
    }
    t += std::max(std::fabs(d), kSurfaceEpsilon);
    if (t > kMaxDist) {
      break;
    }
  }
  return std::nullopt;
}

// Casts a ray from origin along dir (normalized) against every primitive in
// scene, returning the nearest hit, or std::nullopt if nothing is hit.
inline std::optional<SceneRayHit> raycast_scene(const SdfScene &scene,
                                                glm::vec3 origin,
                                                glm::vec3 dir) {
  std::optional<SceneRayHit> best;

  for (int i = 0; i < static_cast<int>(scene.layers.size()); ++i) {
    for (const SdfPrimitiveDef &primitive : scene.layers[i].primitives) {
      std::optional<f32> t = raymarch_primitive(primitive, origin, dir);
      if (t && (!best || *t < best->distance)) {
        best = SceneRayHit{*t, i};
      }
    }
  }

  return best;
}
