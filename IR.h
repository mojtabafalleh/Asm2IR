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

    uint64_t allocate_assign_id() { return ++next_id; }

    void add(std::unique_ptr<Statement> stmt) { statements.push_back(std::move(stmt)); }

    Value* get_assign_dst(uint64_t id) const {
        auto it = assign_map.find(id);
        return it != assign_map.end() ? it->second->dst.get() : nullptr;
    }

    AssignStatement* get_assign_statement(uint64_t id) const {
        auto it = assign_map.find(id);
        return it != assign_map.end() ? it->second : nullptr;
    }

    std::string assign_dst_name(uint64_t id) const {
        auto* dst = get_assign_dst(id);
        if (!dst) return "?";
        if (auto* reg = dynamic_cast<RegValue*>(dst)) return reg->register_name();
        return dynamic_cast<MemoryValue*>(dst) ? "mem" : "?";
    }

    std::string expression_str(Expression* expr) const {
        if (!expr) return "?";

        if (auto* value = dynamic_cast<Value*>(expr))
            return value_str(value);

        if (auto* binary = dynamic_cast<BinaryExpression*>(expr))
            return wrap(binary_op_name(binary->operation()),
                        expression_str(binary->left.get()), expression_str(binary->right.get()));

        if (auto* unary = dynamic_cast<UnaryExpression*>(expr))
            return wrap(unary_op_name(unary->operation()), expression_str(unary->operand.get()));

        if (auto* cond = dynamic_cast<ConditionalExpression*>(expr))
            return "Conditional( " + expression_str(cond->condition.get()) + " ? " +
                   expression_str(cond->true_expr.get()) + ": " +
                   expression_str(cond->false_expr.get()) + " ) ";

        return "?";
    }

    std::string statement_str(Statement* stmt) const {
        if (!stmt) return "?";

        if (auto* assign = dynamic_cast<AssignStatement*>(stmt))
            return "Assign(" + expression_str(assign->dst.get()) + ", " + expression_str(assign->value.get()) + ")";
        if (auto* store = dynamic_cast<StoreStatement*>(stmt))
            return "Store(" + expression_str(store->address.get()) + ", " + expression_str(store->value.get()) + ")";
        if (auto* call = dynamic_cast<CallStatement*>(stmt))
            return "Call(" + addr_str(call->target) + ")";
        if (dynamic_cast<ReturnStatement*>(stmt))
            return "Return()";
        if (auto* interrupt = dynamic_cast<InterruptStatement*>(stmt))
            return "Interrupt(" + std::to_string(static_cast<unsigned>(interrupt->vector)) + ")";

        return "?";
    }

    std::string statements_str() const {
        std::stringstream ss;
        for (const auto& stmt : statements)
            ss << statement_str(stmt.get()) << "\n";
        return ss.str();
    }

private:
    // These two used to be full ~20-case switch statements duplicated
    // inline inside expression_str, just to turn an Operation into the
    // word used in "Add(...)"/"Neg(...)"-style debug output. Kept as
    // two separate tables (rather than one shared one) because
    // Binary/UnaryExpression each only ever carry a subset of
    // Operation, and merging them would silently accept an operator
    // that was never valid for that node kind.
    static const char* binary_op_name(Operation op) {
        switch (op) {
            case Operation::Add: return "Add";       case Operation::Sub: return "Sub";
            case Operation::Mul: return "Mul";       case Operation::Div: return "Div";
            case Operation::Mod: return "Mod";       case Operation::Shl: return "Shl";
            case Operation::Shr: return "Shr";       case Operation::Rcr: return "Rcr";
            case Operation::Sar: return "Sar";       case Operation::Rol: return "Rol";
            case Operation::Ror: return "Ror";       case Operation::Rcl: return "Rcl";
            case Operation::BitAnd: return "BitAnd"; case Operation::BitOr: return "BitOr";
            case Operation::BitXor: return "BitXor";
            case Operation::LogicalAnd: return "LogicalAnd";
            case Operation::LogicalOr: return "LogicalOr";
            case Operation::Eq: return "Eq"; case Operation::Ne: return "Ne";
            case Operation::Lt: return "Lt"; case Operation::Le: return "Le";
            case Operation::Gt: return "Gt"; case Operation::Ge: return "Ge";
            default: return nullptr;
        }
    }

    static const char* unary_op_name(Operation op) {
        switch (op) {
            case Operation::Load: return "Load";
            case Operation::Neg: return "Neg";
            case Operation::BitNot: return "BitNot";
            case Operation::LogicalNot: return "LogicalNot";
            default: return nullptr;
        }
    }

    static std::string wrap(const char* name, const std::string& a) {
        return name ? std::string(name) + "(" + a + ")" : "?";
    }
    static std::string wrap(const char* name, const std::string& a, const std::string& b) {
        return name ? std::string(name) + "(" + a + ", " + b + ")" : "?";
    }

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
                if (mem->scale != 1) ss << "*" << static_cast<int>(mem->scale);
                first = false;
            }
            if (mem->displacement != 0) {
                if (!first) ss << (mem->displacement > 0 ? " + " : " - ");
                ss << "0x" << std::hex << std::uppercase << std::llabs(mem->displacement);
            }
            ss << "]";
            return ss.str();
        }

        return "?";
    }
};