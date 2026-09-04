#pragma once

#include "expression.h"
#include "Statement.h"
#include <vector>
#include <sstream>
#include <string>

// A flat sequence of top-level IR statements for one lifted
// instruction (or, eventually, one basic block). Expression trees
// (values, arithmetic, ...) only ever appear nested inside a
// Statement's operands.
class IR {
public:
    std::vector<std::unique_ptr<Statement>> statements;

    // Append a top-level statement to the IR.
    void add(std::unique_ptr<Statement> stmt) {
        statements.push_back(std::move(stmt));
    }

    // Recursively render a single expression tree as text, e.g.
    // "Assign(rax, 0x1)". Dispatches by dynamic_cast on node kind.
    std::string expression_str(Expression* expr) const {

        if (!expr)
            return "?";


        // --------------------
        // Values
        // --------------------

        if (auto* value = dynamic_cast<Value*>(expr))
            return value_str(value);


        // --------------------
        // Binary Expression
        // --------------------

        if (auto* binary = dynamic_cast<BinaryExpression*>(expr)) {

            const std::string left =
                expression_str(binary->left.get());

            const std::string right =
                expression_str(binary->right.get());

            switch (binary->operation()) {

                case Operation::Add:
                    return "Add(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Sub:
                    return "Sub(" +
                           left +
                           ", " +
                           right +
                           ")";


                case Operation::Mul:
                    return "Mul(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Div:
                    return "Div(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Mod:
                    return "Mod(" +
                           left +
                           ", " +
                           right +
                           ")";


                // --------------------
                // Bitwise
                // --------------------

                case Operation::Shl:
                    return "Shl(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Shr:
                    return "Shr(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Rcr:
                    return "Rcr(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Sar:
                    return "Sar(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Rol:
                    return "Rol(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Ror:
                    return "Ror(" +
                           left +
                           ", " +
                           right +
                           ")";
                case Operation::Rcl:
                    return "Rcl(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::BitAnd:
                    return "BitAnd(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::BitOr:
                    return "BitOr(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::BitXor:
                    return "BitXor(" +
                           left +
                           ", " +
                           right +
                           ")";


                // --------------------
                // Logical
                // --------------------

                case Operation::LogicalAnd:
                    return "LogicalAnd(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::LogicalOr:
                    return "LogicalOr(" +
                           left +
                           ", " +
                           right +
                           ")";


                // --------------------
                // Compare
                // --------------------

                case Operation::Eq:
                    return "Eq(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Ne:
                    return "Ne(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Lt:
                    return "Lt(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Le:
                    return "Le(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Gt:
                    return "Gt(" +
                           left +
                           ", " +
                           right +
                           ")";

                case Operation::Ge:
                    return "Ge(" +
                           left +
                           ", " +
                           right +
                           ")";


                default:
                    return "?";
            }
        }


        // --------------------
        // Unary Expression
        // --------------------

        if (auto* unary = dynamic_cast<UnaryExpression*>(expr)) {

            const std::string operand =
                expression_str(unary->operand.get());

            switch (unary->operation()) {

                case Operation::Load:
                    return "Load(" +
                           operand +
                           ")";

                case Operation::Neg:
                    return "Neg(" +
                           operand +
                           ")";

                case Operation::BitNot:
                    return "BitNot(" +
                           operand +
                           ")";

                case Operation::LogicalNot:
                    return "LogicalNot(" +
                           operand +
                           ")";

                default:
                    return "?";
            }
        }


        return "?";
    }

// Render a single top-level statement as text, e.g.
// "Assign(rax, 0x1)" or "Store([rsp + 0x10], rsi)".
std::string statement_str(Statement* stmt) const {

    if (!stmt)
        return "?";

    if (auto* assign = dynamic_cast<AssignStatement*>(stmt)) {

        return "Assign(" +
               expression_str(assign->dst.get()) +
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

    return "?";
}

// Render every top-level statement, one per line.
std::string statements_str() const {
    std::stringstream ss;

    for (const auto& stmt : statements) {
        ss << statement_str(stmt.get()) << "\n";
    }

    return ss.str();
}

private:

    // Render a leaf Value node (register, immediate, or memory).
    // Note: duplicates formatting logic already present on
    // RegValue/MemoryValue instead of calling their own methods.
    //
    // The one exception is registers: we use RegValue::display_name()
    // instead of register_name(), so the access width is visible
    // right in the IR text, e.g. "rax" for a 64-bit access vs
    // "rax:32" for `mov eax, ...`. Widths always come from the
    // Lifter, which fills them in from Capstone's operand size.
    std::string value_str(Value* value) const {
        if (!value)
            return "?";

        if (auto* reg = dynamic_cast<RegValue*>(value))
            return reg->display_name();

        if (auto* imm = dynamic_cast<ImmValue*>(value)) {
            std::stringstream ss;
            ss << "0x"
               << std::hex
               << std::uppercase
               << imm->value;
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
                if (!first)
                    ss << " + ";

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

                ss << "0x"
                   << std::hex
                   << std::uppercase
                   << std::llabs(mem->displacement);
            }

            ss << "]";
            return ss.str();
        }

        return "?";
    }


};