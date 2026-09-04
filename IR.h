// IR.h
#pragma once

#include "expression.h"
#include "Statement.h"
#include <vector>
#include <sstream>
#include <string>
#include <unordered_map>
#include <memory>

class IR {
public:
    std::vector<std::unique_ptr<Statement>> statements;
    std::unordered_map<uint64_t, AssignStatement*> assign_map;

    // Maps the address of a lifted x86 instruction to the index of the
    // first IR statement it produced. One instruction can lower to
    // several statements (e.g. a push), but jump/call targets always
    // refer to an instruction address, so this is what lets the
    // recompiler know *where* in the statement stream to bind a label
    // for a given target address.
    std::unordered_map<uint64_t, size_t> address_starts;

    uint64_t next_id = 0;

    uint64_t allocate_assign_id() {
        return ++next_id;
    }

    // Called by the Lifter right before lifting each instruction.
    void mark_instruction_start(uint64_t address) {
        address_starts[address] = statements.size();
    }

    void add(std::unique_ptr<Statement> stmt) {
        if (auto* assign = dynamic_cast<AssignStatement*>(stmt.get())) {
            assign_map[assign->id] = assign;
        }
        statements.push_back(std::move(stmt));
    }

    Value* get_assign_dst(uint64_t id) const {
        auto it = assign_map.find(id);
        if (it != assign_map.end())
            return it->second->dst.get();
        return nullptr;
    }

    AssignStatement* get_assign_statement(uint64_t id) const {
        auto it = assign_map.find(id);
        if (it != assign_map.end())
            return it->second;
        return nullptr;
    }

    std::string assign_dst_name(uint64_t id) const {
        auto* dst = get_assign_dst(id);
        if (!dst) return "?";
        if (auto* reg = dynamic_cast<RegValue*>(dst))
            return reg->register_name();
        if (auto* mem = dynamic_cast<MemoryValue*>(dst))
            return "mem";
        return "?";
    }

    std::string expression_str(Expression* expr) const {
        if (!expr) return "?";

        if (auto* value = dynamic_cast<Value*>(expr))
            return value_str(value);

        if (auto* binary = dynamic_cast<BinaryExpression*>(expr)) {
            const std::string left = expression_str(binary->left.get());
            const std::string right = expression_str(binary->right.get());
            switch (binary->operation()) {
                case Operation::Add: return "Add(" + left + ", " + right + ")";
                case Operation::Sub: return "Sub(" + left + ", " + right + ")";
                case Operation::Mul: return "Mul(" + left + ", " + right + ")";
                case Operation::Div: return "Div(" + left + ", " + right + ")";
                case Operation::Mod: return "Mod(" + left + ", " + right + ")";
                case Operation::Shl: return "Shl(" + left + ", " + right + ")";
                case Operation::Shr: return "Shr(" + left + ", " + right + ")";
                case Operation::Rcr: return "Rcr(" + left + ", " + right + ")";
                case Operation::Sar: return "Sar(" + left + ", " + right + ")";
                case Operation::Rol: return "Rol(" + left + ", " + right + ")";
                case Operation::Ror: return "Ror(" + left + ", " + right + ")";
                case Operation::Rcl: return "Rcl(" + left + ", " + right + ")";
                case Operation::BitAnd: return "BitAnd(" + left + ", " + right + ")";
                case Operation::BitOr: return "BitOr(" + left + ", " + right + ")";
                case Operation::BitXor: return "BitXor(" + left + ", " + right + ")";
                case Operation::LogicalAnd: return "LogicalAnd(" + left + ", " + right + ")";
                case Operation::LogicalOr: return "LogicalOr(" + left + ", " + right + ")";
                case Operation::Eq: return "Eq(" + left + ", " + right + ")";
                case Operation::Ne: return "Ne(" + left + ", " + right + ")";
                case Operation::Lt: return "Lt(" + left + ", " + right + ")";
                case Operation::Le: return "Le(" + left + ", " + right + ")";
                case Operation::Gt: return "Gt(" + left + ", " + right + ")";
                case Operation::Ge: return "Ge(" + left + ", " + right + ")";
                default: return "?";
            }
        }

        if (auto* unary = dynamic_cast<UnaryExpression*>(expr)) {
            const std::string operand = expression_str(unary->operand.get());
            switch (unary->operation()) {
                case Operation::Load: return "Load(" + operand + ")";
                case Operation::Neg: return "Neg(" + operand + ")";
                case Operation::BitNot: return "BitNot(" + operand + ")";
                case Operation::LogicalNot: return "LogicalNot(" + operand + ")";
                default: return "?";
            }
        }

        if (auto* cond = dynamic_cast<ConditionalExpression*>(expr)) {
            return "Conditional(" +
                   expression_str(cond->condition.get()) + ", " +
                   expression_str(cond->true_expr.get()) + ", " +
                   expression_str(cond->false_expr.get()) + ")";
        }

        return "?";
    }

    std::string statement_str(Statement* stmt) const {
        if (!stmt) return "?";
        if (auto* assign = dynamic_cast<AssignStatement*>(stmt)) {
            return "Assign(" +
                   expression_str(assign->dst.get()) +
                   "[" + std::to_string(assign->id) + "]" +
                   ", " +
                   expression_str(assign->value.get()) +
                   ")";
        }
        if (auto* store = dynamic_cast<StoreStatement*>(stmt)) {
            return "Store(" +
                   expression_str(store->address.get()) +
                   ", " +
                   expression_str(store->value.get()) +
                   ")";
        }
        if (auto* jump = dynamic_cast<JumpStatement*>(stmt)) {
            return "Jump(" + addr_str(jump->target) + ")";
        }
        if (auto* cjump = dynamic_cast<ConditionalJumpStatement*>(stmt)) {
            return "JumpIf(" +
                   expression_str(cjump->condition.get()) +
                   ", " + addr_str(cjump->target) + ")";
        }
        if (auto* call = dynamic_cast<CallStatement*>(stmt)) {
            return "Call(" + addr_str(call->target) + ")";
        }
        if (dynamic_cast<ReturnStatement*>(stmt)) {
            return "Return()";
        }
        if (auto* interrupt = dynamic_cast<InterruptStatement*>(stmt)) {
            return "Interrupt(" +
                   std::to_string(static_cast<unsigned>(interrupt->vector)) +
                   ")";
        }
        return "?";
    }

    std::string statements_str() const {
        std::stringstream ss;
        for (const auto& stmt : statements) {
            ss << statement_str(stmt.get()) << "\n";
        }
        return ss.str();
    }

private:
    std::string addr_str(uint64_t addr) const {
        std::stringstream ss;
        ss << "0x" << std::hex << std::uppercase << addr;
        return ss.str();
    }

    std::string value_str(Value* value) const {
        if (!value) return "?";
        if (auto* reg = dynamic_cast<RegValue*>(value))
            return reg->display_name();
        if (auto* imm = dynamic_cast<ImmValue*>(value)) {
            std::stringstream ss;
            ss << "0x" << std::hex << std::uppercase << imm->value;
            return ss.str();
        }
        if (auto* mem = dynamic_cast<MemoryValue*>(value)) {
            std::stringstream ss;
            ss << "[";
            bool first = true;
            if (mem->base) {
                ss << value_str(mem->base.get());
                first = false;
            }
            if (mem->index) {
                if (!first) ss << " + ";
                ss << value_str(mem->index.get());
                if (mem->scale != 1)
                    ss << "*" << static_cast<int>(mem->scale);
                first = false;
            }
            if (mem->displacement != 0) {
                if (!first) {
                    if (mem->displacement > 0)
                        ss << " + ";
                    else
                        ss << " - ";
                }
                ss << "0x" << std::hex << std::uppercase
                   << std::llabs(mem->displacement);
            }
            ss << "]";
            return ss.str();
        }
        if (auto* ref = dynamic_cast<AssignRef*>(value)) {
            auto* dst = get_assign_dst(ref->id);
            if (!dst) return "?";
            std::string name;
            if (auto* reg = dynamic_cast<RegValue*>(dst))
                name = reg->register_name();
            else if (dynamic_cast<MemoryValue*>(dst))
                name = "mem";
            else
                name = "?";
            return name + "[" + std::to_string(ref->id) + "]";
        }
        return "?";
    }
};