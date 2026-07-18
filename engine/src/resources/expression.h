#pragma once
#include "../defines.h"

#include <glm/glm.hpp>
#include <optional>
#include <string_view>
#include <vector>

// Compiles a small arithmetic-expression language into a compact bytecode
// form ("parametric attributes" -- a primitive parameter authored as a
// formula instead of a fixed number, e.g. width = "0.1 + 0.1*p.y" so a box
// tapers along its own local Y axis). The same bytecode is interpreted in
// two independent places: evaluate_expression() below (CPU, used by the SDF
// editor's ray_intersect.h for click-picking) and a hand-written mirror in
// Builtin.RaymarchVoxelize.comp.glsl's evaluate_expr() (GPU, used to bake
// the voxel field) -- compiling once here and interpreting bytecode
// everywhere else means a new primitive parameter never needs its own
// bespoke formula-evaluation code, on either side.
//
// Grammar (standard precedence, left-associative):
//   expression := term (('+' | '-') term)*
//   term       := unary (('*' | '/') unary)*
//   unary      := ('-' | '+') unary | primary
//   primary    := NUMBER
//               | 'p' '.' ('x' | 'y' | 'z')
//               | IDENT '(' expression (',' expression)* ')'
//               | '(' expression ')'
//
// Variables: p.x/p.y/p.z, the local-space point primitive_sdf() is
// evaluating at (same local space params.xyz/extra_param already live in --
// world position subtracted, then rotated by the primitive's inverse
// rotation, same as everywhere else in this primitive). Bare identifiers
// other than "p.x"/"p.y"/"p.z" are always function calls (must be followed
// by '('); there is no "just x" shorthand and no other named constant.
//
// Functions (matching GLSL's own semantics exactly, including guarding
// sqrt's domain the same way e.g. round_cone_sdf() already does elsewhere
// in this codebase): abs(a), sin(a), cos(a), sqrt(a), min(a,b), max(a,b),
// pow(a,b), clamp(a,lo,hi).
enum class ExprOp : i32 {
  Const = 0,
  VarX = 1,
  VarY = 2,
  VarZ = 3,
  Add = 4,
  Sub = 5,
  Mul = 6,
  Div = 7,
  Neg = 8,
  Abs = 9,
  Sin = 10,
  Cos = 11,
  Sqrt = 12,
  Min = 13,
  Max = 14,
  Pow = 15,
  Clamp = 16,
};

// Cap on a single compiled expression's instruction count. Must match
// MAX_EXPR_INSTRUCTIONS in Builtin.RaymarchVoxelize.comp.glsl exactly --
// GpuParamExpr (VulkanRaymarchShader, engine-side) allocates fixed-size
// op/operand arrays of exactly this length, mirrored by ParamExpr
// (GLSL-side). Generous for the kind of short procedural-modulation
// formulas this is meant for (e.g. "0.1 + 0.1*p.y" is 5 instructions);
// compile_expression() fails (logs and returns nullopt) rather than
// silently truncating a longer one.
constexpr u32 kMaxExprInstructions = 16;

struct ExprInstruction {
  ExprOp op = ExprOp::Const;
  f32 operand = 0.0f; // Only meaningful for Const.
};

struct CompiledExpression {
  std::vector<ExprInstruction> instructions; // size() <= kMaxExprInstructions.
};

// Parses and compiles source (see the grammar/variables/functions above).
// Returns std::nullopt on a syntax error (unknown token, wrong arg count,
// unmatched parenthesis, trailing garbage, etc.) or an expression needing
// more than kMaxExprInstructions -- logged either way, so a caller can
// safely treat nullopt as "fall back to this parameter's plain constant"
// (see SdfPrimitiveDef::param_expressions).
std::optional<CompiledExpression> compile_expression(std::string_view source);

// Runs expr at local-space point p (p.x/p.y/p.z feed the expression's
// p.x/p.y/p.z variables) and returns the result. Safe to call every sample
// point of a raymarch -- this is pure bytecode interpretation, no parsing.
f32 evaluate_expression(const CompiledExpression &expr, glm::vec3 p);
