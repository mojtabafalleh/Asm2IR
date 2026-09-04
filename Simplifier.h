#pragma once

#include "expression.h"
#include "Statement.h"
#include "IR.h"
#include <memory>
#include <limits>

// A minimal, rule-based IR simplifier. Deliberately tiny for now:
// it walks every expression tree in the IR and rewrites nodes that
// match a known-safe pattern.
//
// Current rules:
//   1) X ^ X -> 0        (XOR of a register/immediate/memory
//                          operand with itself always yields zero)
//   2) X + (-C) -> X - C  (adding a negative immediate is the same
//                          as subtracting its absolute value; makes
//                          e.g. `Add(rsp, 0xFFFFFFFFFFFFFFF8)` read
//                          as `Sub(rsp, 0x8)` instead)
//
// Adding a new rule later just means adding another check inside
// simplify_expr() below.
class Simplifier {
public:
    // Simplify every statement in the IR, in place.
    void simplify(IR& ir) {
        for (auto& stmt : ir.statements)
            simplify_statement(stmt.get());
    }

private:
    bool is_all_ones(uint64_t value, uint8_t width) const {
        if (width == 0 || width > 64) return false;
        if (width == 64) return value == UINT64_MAX;
        return value == (uint64_t(1) << width) - 1;
    }

    void simplify_statement(Statement* stmt) {
        if (!stmt) return;
        if (auto* assign = dynamic_cast<AssignStatement*>(stmt)) { simplify_expr(assign->value); return; }
        if (auto* store = dynamic_cast<StoreStatement*>(stmt)) { simplify_expr(store->value); return; }
        if (auto* cjump = dynamic_cast<ConditionalJumpStatement*>(stmt)) { simplify_expr(cjump->condition); return; }
        // Jump / Call / Return / Interrupt carry no expressions to simplify.
    }

    // Recursively simplifies `expr` in place. `expr` is passed as the
    // owning unique_ptr slot (not a raw pointer) so a node can be
    // *replaced* with a brand new one -- e.g. an entire
    // BinaryExpression(BitXor, x, x) subtree collapsing into a single
    // ImmValue(0).
    void simplify_expr(std::unique_ptr<Expression>& expr) {
        if (!expr) return;

        if (auto* binary = dynamic_cast<BinaryExpression*>(expr.get())) {
            // Simplify children first (bottom-up), so nested self-xors
            // fold before we check this node.
            simplify_expr(binary->left);
            simplify_expr(binary->right);

            if (binary->operation() == Operation::BitXor) {
                // Rule 1a: reg ^ all_ones -> LogicalNot(reg)
                auto* reg = dynamic_cast<RegValue*>(binary->left.get());
                auto* imm = dynamic_cast<ImmValue*>(binary->right.get());
                if (reg && imm && is_all_ones(imm->value, reg->width)) {
                    expr = std::make_unique<UnaryExpression>(Operation::LogicalNot, std::move(binary->left));
                    return;
                }
                // Rule 1b: X ^ X -> 0
                if (same_value(binary->left.get(), binary->right.get())) {
                    expr = std::make_unique<ImmValue>(0);
                    return;
                }
            }

            // Rule 2: X + (-C) -> X - C
            //
            // ImmValue::value is stored as a raw uint64_t, so a "negative"
            // immediate is one whose bit pattern, read as signed 64-bit
            // two's complement, is < 0 (e.g. the Lifter/Capstone hands us
            // "add rsp, -8" as Add(rsp, 0xFFFFFFFFFFFFFFF8)). Rewriting the
            // operation to Sub and negating the immediate back to its
            // positive magnitude keeps the value identical while making
            // the IR read the way a human would write it.
            if (binary->operation() == Operation::Add) {
                if (auto* imm = dynamic_cast<ImmValue*>(binary->right.get())) {
                    const int64_t signed_value = static_cast<int64_t>(imm->value);
                    // INT64_MIN has no positive two's-complement counterpart
                    // (negating it overflows), so it's deliberately left alone.
                    if (signed_value < 0 && signed_value != std::numeric_limits<int64_t>::min()) {
                        binary->op = Operation::Sub;
                        binary->right = std::make_unique<ImmValue>(-signed_value);
                    }
                }
            }

            // Rule 3: X - X -> 0
            if (binary->operation() == Operation::Sub &&
                same_value(binary->left.get(), binary->right.get())) {
                expr = std::make_unique<ImmValue>(0);
                return;
            }
            return;
        }

        if (auto* unary = dynamic_cast<UnaryExpression*>(expr.get())) {
            simplify_expr(unary->operand);
            return;
        }

        // Value leaves (register / immediate / memory) have nothing
        // further to simplify.
    }

    // Structural equality between two operands, used to detect "X ^ X"
    // / "X - X". Only Value-kind nodes are compared (register,
    // immediate, memory).
    bool same_value(Expression* a, Expression* b) const {
        auto* va = dynamic_cast<Value*>(a);
        auto* vb = dynamic_cast<Value*>(b);
        if (!va || !vb) return false;

        if (auto* ra = dynamic_cast<RegValue*>(va)) {
            auto* rb = dynamic_cast<RegValue*>(vb);
            return rb && ra->reg == rb->reg && ra->width == rb->width;
        }
        if (auto* ia = dynamic_cast<ImmValue*>(va)) {
            auto* ib = dynamic_cast<ImmValue*>(vb);
            return ib && ia->value == ib->value;
        }
        if (auto* ma = dynamic_cast<MemoryValue*>(va)) {
            auto* mb = dynamic_cast<MemoryValue*>(vb);
            if (!mb) return false;
            if (ma->size != mb->size || ma->scale != mb->scale || ma->displacement != mb->displacement)
                return false;
            return same_optional(ma->base.get(), mb->base.get()) &&
                   same_optional(ma->index.get(), mb->index.get());
        }
        return false;
    }

    // Like same_value, but treats "both null" as equal -- used for
    // MemoryValue::base/index, which are optional.
    bool same_optional(Value* a, Value* b) const {
        if (!a && !b) return true;
        if (!a || !b) return false;
        return same_value(a, b);
    }
};