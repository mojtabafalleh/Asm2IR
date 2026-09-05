// RegisterAst.h
//
// Given an IR (a vector<unique_ptr<Statement>> produced by the lifter),
// reconstructs, for any register version, the full symbolic expression
// that computes it in terms of the *original* input registers/flags
// (SSA id == the value the register held before this lifted block
// started).
//
// Resolution is done by *position in the statement stream*, not by the
// printed SSA number: for a use of `reg` found while expanding the
// statement at index `at`, we look for the last Assign to `reg` whose
// index is < at. If none exists, that use is the register's original,
// unmodified input value.
//
// Memory reads (a bare MemoryValue used as an expression, i.e. a Load)
// are resolved the same way: the address is fully resolved to a
// canonical expression and matched, in reverse program order, against
// the resolved addresses of prior Store statements. If nothing matches,
// a symbolic Load(...) node is kept.
//
// Output: build()/build_last() return a brand-new IR object containing
// a single AssignStatement (dst = the queried register, value = the
// resolved expression). Print it however you already print IR, e.g.
// `result.statements_str()`.
//
// Known limitations (v1):
//  - Assumes straight-line IR (no control-flow merges / phi nodes).
//  - Memory aliasing is exact-match only (same base/index/scale/disp
//    after resolution). Partial overlaps are not detected.
//  - No sub-register width modeling; RegValue::width is carried along
//    for display but not used for correctness.

#pragma once

#include "IR.h"
#include "expression.h"
#include "Statement.h"

#include <vector>
#include <unordered_map>
#include <memory>
#include <stdexcept>

class RegisterAst {
public:
    explicit RegisterAst(const IR& ir) : ir_(ir) {
        for (size_t i = 0; i < ir_.statements.size(); ++i) {
            Statement* s = ir_.statements[i].get();
            if (s->kind() == StatementKind::Assign) {
                auto* as = static_cast<AssignStatement*>(s);
                if (auto* rv = dynamic_cast<RegValue*>(as->dst.get())) {
                    defs_by_reg_[rv->reg].push_back({ i, as });
                }
            } else if (s->kind() == StatementKind::Store) {
                auto* ss = static_cast<StoreStatement*>(s);
                if (auto* mv = dynamic_cast<MemoryValue*>(ss->address.get())) {
                    stores_.push_back({ i, mv, ss->value.get() });
                }
            }
        }
    }

    // Resolves the last write to `reg` in the whole IR.
    // Returns a new IR containing a single: Assign(reg[N], <resolved expr>)
    IR build_last(Reg reg) {
        auto it = defs_by_reg_.find(reg);
        std::unique_ptr<Expression> resolved;
        int ssa = 0;
        if (it == defs_by_reg_.end() || it->second.empty()) {
            resolved = make_input_leaf(reg);
            ssa = 0;
        } else {
            auto& d = it->second.back();
            auto* dst_rv = static_cast<RegValue*>(d.stmt->dst.get());
            ssa = dst_rv->ssa_id;
            resolved = resolve(d.stmt->value.get(), d.index, 0);
        }
        return wrap(reg, ssa, std::move(resolved));
    }

    // Resolves the write to `reg` whose destination SSA id == target.SSA_id.
    // (`target.reg` and `target.SSA_id` are the only fields used.)
    IR build(const RegValue& target) {
        auto it = defs_by_reg_.find(target.reg);
        if (it != defs_by_reg_.end()) {
            for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
                auto* rv = static_cast<RegValue*>(rit->stmt->dst.get());
                if (rv->ssa_id == target.ssa_id) {
                    auto resolved = resolve(rit->stmt->value.get(), rit->index, 0);
                    return wrap(target.reg, target.ssa_id, std::move(resolved));
                }
            }
        }
        // no such definition -> it's the input value
        return wrap(target.reg, target.ssa_id, make_input_leaf(target.reg));
    }

private:
    struct DefEntry { size_t index; AssignStatement* stmt; };
    struct StoreEntry { size_t index; MemoryValue* addr; Expression* value; };

    const IR& ir_;
    std::unordered_map<Reg, std::vector<DefEntry>> defs_by_reg_;
    std::vector<StoreEntry> stores_;

    static std::unique_ptr<RegValue> make_input_leaf(Reg reg) {
        auto rv = std::make_unique<RegValue>(reg, 64);
        rv->ssa_id = 0; // convention: id 0 == value on entry to this IR block
        return rv;
    }

    static IR wrap(Reg reg, int ssa, std::unique_ptr<Expression> value) {
        IR out;
        auto dst = std::make_unique<RegValue>(reg, 64);
        dst->ssa_id = ssa;
        out.add(std::make_unique<AssignStatement>(0, std::move(dst), std::move(value)));
        return out;
    }

    // Last definition of `reg` with index strictly before `before_index`.
    DefEntry* def_before(Reg reg, size_t before_index) {
        auto it = defs_by_reg_.find(reg);
        if (it == defs_by_reg_.end()) return nullptr;
        DefEntry* best = nullptr;
        for (auto& d : it->second) {
            if (d.index < before_index) best = &d;
            else break; // vector is in ascending program order
        }
        return best;
    }

    std::unique_ptr<Expression> resolve(Expression* e, size_t at_index, int depth) {
        if (depth > 256)
            throw std::runtime_error("RegisterAst: expression too deep (loop/back-edge in IR?)");

        if (auto* rv = dynamic_cast<RegValue*>(e)) {
            auto* def = def_before(rv->reg, at_index);
            if (!def) return make_input_leaf(rv->reg);
            return resolve(def->stmt->value.get(), def->index, depth + 1);
        }
        if (auto* iv = dynamic_cast<ImmValue*>(e)) {
            return iv->clone();
        }
        if (auto* mv = dynamic_cast<MemoryValue*>(e)) {
            return resolve_load(mv, at_index, depth);
        }
        if (auto* ue = dynamic_cast<UnaryExpression*>(e)) {
            return std::make_unique<UnaryExpression>(ue->op, resolve(ue->operand.get(), at_index, depth + 1));
        }
        if (auto* be = dynamic_cast<BinaryExpression*>(e)) {
            return std::make_unique<BinaryExpression>(be->op,
                resolve(be->left.get(), at_index, depth + 1),
                resolve(be->right.get(), at_index, depth + 1));
        }
        if (auto* ce = dynamic_cast<ConditionalExpression*>(e)) {
            return std::make_unique<ConditionalExpression>(
                resolve(ce->condition.get(), at_index, depth + 1),
                resolve(ce->true_expr.get(), at_index, depth + 1),
                resolve(ce->false_expr.get(), at_index, depth + 1));
        }
        throw std::runtime_error("RegisterAst: unhandled expression node");
    }

    // Builds a canonical, fully-resolved address expression out of a
    // MemoryValue's base/index/scale/displacement so two addresses can be
    // compared structurally regardless of how they were computed.
    std::unique_ptr<Expression> resolve_address(MemoryValue* mv, size_t at_index, int depth) {
        std::unique_ptr<Expression> addr; // stays null until first term is added

        if (mv->base) {
            addr = resolve(mv->base.get(), at_index, depth + 1);
        }

        if (mv->index) {
            std::unique_ptr<Expression> idx = resolve(mv->index.get(), at_index, depth + 1);
            if (mv->scale != 1) {
                std::unique_ptr<Expression> scaled = std::make_unique<BinaryExpression>(
                    Operation::Mul, std::move(idx),
                    std::make_unique<ImmValue>(static_cast<int64_t>(mv->scale)));
                idx = std::move(scaled);
            }
            if (addr) {
                std::unique_ptr<Expression> sum = std::make_unique<BinaryExpression>(
                    Operation::Add, std::move(addr), std::move(idx));
                addr = std::move(sum);
            } else {
                addr = std::move(idx);
            }
        }

        if (mv->displacement != 0) {
            std::unique_ptr<Expression> disp = std::make_unique<ImmValue>(mv->displacement);
            if (addr) {
                std::unique_ptr<Expression> sum = std::make_unique<BinaryExpression>(
                    Operation::Add, std::move(addr), std::move(disp));
                addr = std::move(sum);
            } else {
                addr = std::move(disp);
            }
        }

        if (!addr) {
            addr = std::make_unique<ImmValue>(0);
        }
        return addr;
    }

    std::unique_ptr<Expression> resolve_load(MemoryValue* mv, size_t at_index, int depth) {
        auto want_addr = resolve_address(mv, at_index, depth);

        for (auto it = stores_.rbegin(); it != stores_.rend(); ++it) {
            if (it->index >= at_index) continue;
            if (it->addr->size != mv->size) continue;
            auto store_addr = resolve_address(it->addr, it->index, depth + 1);
            if (same_expr(want_addr.get(), store_addr.get())) {
                return resolve(it->value, it->index, depth + 1);
            }
        }
        // Unresolved: keep a symbolic Load over the (already-simplified) address.
        return std::make_unique<UnaryExpression>(Operation::Load, std::move(want_addr));
    }

    static bool same_expr(const Expression* a, const Expression* b) {
        if (!a || !b) return a == b;
        if (auto* ra = dynamic_cast<const RegValue*>(a)) {
            auto* rb = dynamic_cast<const RegValue*>(b);
            return rb && ra->reg == rb->reg && ra->ssa_id == rb->ssa_id;
        }
        if (auto* ia = dynamic_cast<const ImmValue*>(a)) {
            auto* ib = dynamic_cast<const ImmValue*>(b);
            return ib && ia->value == ib->value;
        }
        if (auto* ua = dynamic_cast<const UnaryExpression*>(a)) {
            auto* ub = dynamic_cast<const UnaryExpression*>(b);
            return ub && ua->op == ub->op && same_expr(ua->operand.get(), ub->operand.get());
        }
        if (auto* ba = dynamic_cast<const BinaryExpression*>(a)) {
            auto* bb = dynamic_cast<const BinaryExpression*>(b);
            return bb && ba->op == bb->op &&
                   same_expr(ba->left.get(), bb->left.get()) &&
                   same_expr(ba->right.get(), bb->right.get());
        }
        if (auto* ca = dynamic_cast<const ConditionalExpression*>(a)) {
            auto* cb = dynamic_cast<const ConditionalExpression*>(b);
            return cb &&
                   same_expr(ca->condition.get(), cb->condition.get()) &&
                   same_expr(ca->true_expr.get(), cb->true_expr.get()) &&
                   same_expr(ca->false_expr.get(), cb->false_expr.get());
        }
        return false; // two still-unresolved MemoryValue leaves etc.: be conservative
    }
};