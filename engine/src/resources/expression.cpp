#include "expression.h"
#include "../core/logger.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

enum class TokenType {
  Number,
  Ident,
  Plus,
  Minus,
  Star,
  Slash,
  LParen,
  RParen,
  Comma,
  Dot,
  End,
  Invalid,
};

struct Token {
  TokenType type = TokenType::End;
  f32 number = 0.0f;
  std::string ident;
};

Token next_token(std::string_view src, size_t &pos) {
  while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos]))) {
    ++pos;
  }
  if (pos >= src.size()) {
    return {TokenType::End};
  }

  char c = src[pos];
  if (std::isdigit(static_cast<unsigned char>(c))) {
    size_t start = pos;
    bool seen_dot = false;
    while (pos < src.size() &&
          (std::isdigit(static_cast<unsigned char>(src[pos])) ||
           (src[pos] == '.' && !seen_dot))) {
      seen_dot = seen_dot || src[pos] == '.';
      ++pos;
    }
    Token token{TokenType::Number};
    token.number = std::stof(std::string(src.substr(start, pos - start)));
    return token;
  }
  if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
    size_t start = pos;
    while (pos < src.size() && (std::isalnum(static_cast<unsigned char>(src[pos])) ||
                                src[pos] == '_')) {
      ++pos;
    }
    return {TokenType::Ident, 0.0f, std::string(src.substr(start, pos - start))};
  }

  ++pos;
  switch (c) {
  case '+':
    return {TokenType::Plus};
  case '-':
    return {TokenType::Minus};
  case '*':
    return {TokenType::Star};
  case '/':
    return {TokenType::Slash};
  case '(':
    return {TokenType::LParen};
  case ')':
    return {TokenType::RParen};
  case ',':
    return {TokenType::Comma};
  case '.':
    return {TokenType::Dot};
  default:
    return {TokenType::Invalid};
  }
}

// A one-pass recursive-descent compiler: emits ExprInstruction directly
// while parsing (standard technique for a stack-machine target -- there's
// no need for a separate AST when every construct maps straight onto
// "evaluate operands, then emit the operator").
class Compiler {
public:
  explicit Compiler(std::string_view source) : source_(source) {
    advance();
  }

  bool ok() const noexcept { return !error_; }
  bool at_end() const noexcept { return current_.type == TokenType::End; }
  std::vector<ExprInstruction> take_instructions() { return std::move(out_); }

  void parse_expression() {
    parse_term();
    while (!error_ &&
          (current_.type == TokenType::Plus || current_.type == TokenType::Minus)) {
      TokenType op = current_.type;
      advance();
      parse_term();
      emit(op == TokenType::Plus ? ExprOp::Add : ExprOp::Sub);
    }
  }

private:
  void advance() { current_ = next_token(source_, pos_); }

  bool expect(TokenType type) {
    if (error_ || current_.type != type) {
      error_ = true;
      return false;
    }
    advance();
    return true;
  }

  void emit(ExprOp op, f32 operand = 0.0f) {
    if (error_) {
      return;
    }
    if (out_.size() >= kMaxExprInstructions) {
      error_ = true;
      return;
    }
    out_.push_back({op, operand});
  }

  void parse_term() {
    parse_unary();
    while (!error_ &&
          (current_.type == TokenType::Star || current_.type == TokenType::Slash)) {
      TokenType op = current_.type;
      advance();
      parse_unary();
      emit(op == TokenType::Star ? ExprOp::Mul : ExprOp::Div);
    }
  }

  void parse_unary() {
    if (current_.type == TokenType::Minus) {
      advance();
      parse_unary();
      emit(ExprOp::Neg);
      return;
    }
    if (current_.type == TokenType::Plus) {
      advance();
      parse_unary(); // unary '+' is a no-op
      return;
    }
    parse_primary();
  }

  // Parses one comma-separated argument list of exactly `arity` expressions
  // (caller has already consumed the function name and is at '(').
  void parse_call_args(int arity) {
    if (!expect(TokenType::LParen)) {
      return;
    }
    for (int i = 0; i < arity; ++i) {
      if (i > 0 && !expect(TokenType::Comma)) {
        return;
      }
      parse_expression();
    }
    expect(TokenType::RParen);
  }

  void parse_primary() {
    if (error_) {
      return;
    }
    if (current_.type == TokenType::Number) {
      emit(ExprOp::Const, current_.number);
      advance();
      return;
    }
    if (current_.type == TokenType::LParen) {
      advance();
      parse_expression();
      expect(TokenType::RParen);
      return;
    }
    if (current_.type != TokenType::Ident) {
      error_ = true;
      return;
    }

    std::string name = current_.ident;
    advance();

    if (name == "p") {
      if (!expect(TokenType::Dot)) {
        return;
      }
      if (current_.type != TokenType::Ident || current_.ident.size() != 1) {
        error_ = true;
        return;
      }
      char axis = current_.ident[0];
      advance();
      if (axis == 'x') {
        emit(ExprOp::VarX);
      } else if (axis == 'y') {
        emit(ExprOp::VarY);
      } else if (axis == 'z') {
        emit(ExprOp::VarZ);
      } else {
        error_ = true;
      }
      return;
    }

    if (name == "abs" || name == "sin" || name == "cos" || name == "sqrt") {
      parse_call_args(1);
      emit(name == "abs"    ? ExprOp::Abs
          : name == "sin"  ? ExprOp::Sin
          : name == "cos"  ? ExprOp::Cos
                            : ExprOp::Sqrt);
      return;
    }
    if (name == "min" || name == "max" || name == "pow") {
      parse_call_args(2);
      emit(name == "min" ? ExprOp::Min : name == "max" ? ExprOp::Max : ExprOp::Pow);
      return;
    }
    if (name == "clamp") {
      parse_call_args(3);
      emit(ExprOp::Clamp);
      return;
    }

    error_ = true; // unknown identifier
  }

  std::string_view source_;
  size_t pos_ = 0;
  Token current_;
  bool error_ = false;
  std::vector<ExprInstruction> out_;
};

} // namespace

std::optional<CompiledExpression> compile_expression(std::string_view source) {
  Compiler compiler(source);
  compiler.parse_expression();

  if (!compiler.ok() || !compiler.at_end()) {
    KWARN("Failed to compile expression '{}': syntax error or too complex "
         "(max {} instructions).",
         source, kMaxExprInstructions);
    return std::nullopt;
  }

  CompiledExpression result;
  result.instructions = compiler.take_instructions();
  return result;
}

f32 evaluate_expression(const CompiledExpression &expr, glm::vec3 p) {
  // kMaxExprInstructions is also a safe upper bound on stack depth: every
  // instruction pushes at most one value net (even a binary op, which pops
  // 2 and pushes 1, never grows the stack), so it can never hold more
  // values than there are instructions.
  f32 stack[kMaxExprInstructions];
  int sp = 0;

  for (const ExprInstruction &instr : expr.instructions) {
    switch (instr.op) {
    case ExprOp::Const:
      stack[sp++] = instr.operand;
      break;
    case ExprOp::VarX:
      stack[sp++] = p.x;
      break;
    case ExprOp::VarY:
      stack[sp++] = p.y;
      break;
    case ExprOp::VarZ:
      stack[sp++] = p.z;
      break;
    case ExprOp::Neg:
      stack[sp - 1] = -stack[sp - 1];
      break;
    case ExprOp::Abs:
      stack[sp - 1] = std::fabs(stack[sp - 1]);
      break;
    case ExprOp::Sin:
      stack[sp - 1] = std::sin(stack[sp - 1]);
      break;
    case ExprOp::Cos:
      stack[sp - 1] = std::cos(stack[sp - 1]);
      break;
    case ExprOp::Sqrt:
      stack[sp - 1] = std::sqrt(std::max(stack[sp - 1], 0.0f));
      break;
    case ExprOp::Add:
      --sp;
      stack[sp - 1] = stack[sp - 1] + stack[sp];
      break;
    case ExprOp::Sub:
      --sp;
      stack[sp - 1] = stack[sp - 1] - stack[sp];
      break;
    case ExprOp::Mul:
      --sp;
      stack[sp - 1] = stack[sp - 1] * stack[sp];
      break;
    case ExprOp::Div:
      --sp;
      stack[sp - 1] = stack[sp - 1] / stack[sp];
      break;
    case ExprOp::Min:
      --sp;
      stack[sp - 1] = std::min(stack[sp - 1], stack[sp]);
      break;
    case ExprOp::Max:
      --sp;
      stack[sp - 1] = std::max(stack[sp - 1], stack[sp]);
      break;
    case ExprOp::Pow:
      --sp;
      stack[sp - 1] = std::pow(stack[sp - 1], stack[sp]);
      break;
    case ExprOp::Clamp:
      sp -= 2;
      stack[sp - 1] = std::clamp(stack[sp - 1], stack[sp], stack[sp + 1]);
      break;
    }
  }

  return sp > 0 ? stack[0] : 0.0f;
}
