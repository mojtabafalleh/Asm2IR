#pragma once

#include "expression.h"
#include <memory>

// Coarse classification for Statement nodes.
enum class StatementKind {
    Assign,
    Store
};


// A Statement has a side effect: it writes a value somewhere
// (a register/flag, or memory). It produces no value itself,
// which is what separates it from Expression.
//
// This mirrors the Expr/Stmt split used by VEX IR (IRExpr vs
// IRStmt) and miasm (ExprXxx vs ExprAssign). Keeping side effects
// out of the expression tree means a pass that only cares about
// *values* (constant folding, an expression simplifier, ...)
// never has to special-case "is this node secretly also a write".
// IR::statements is the top-level list; Expression trees only
// ever appear nested inside a Statement (or inside each other).
class Statement {
public:
    virtual ~Statement() = default;

    virtual StatementKind kind() const = 0;
};


// dst = value
// dst is a register or flag (RegValue). Memory destinations are
// StoreStatement instead, never AssignStatement.
class AssignStatement : public Statement {
public:
    std::unique_ptr<Value> dst;
    std::unique_ptr<Expression> value;

    AssignStatement(
        std::unique_ptr<Value> dst,
        std::unique_ptr<Expression> value
    )
        : dst(std::move(dst)),
          value(std::move(value)) {}

    StatementKind kind() const override {
        return StatementKind::Assign;
    }
};


// *address = value
// address is a MemoryValue.
class StoreStatement : public Statement {
public:
    std::unique_ptr<Value> address;
    std::unique_ptr<Expression> value;

    StoreStatement(
        std::unique_ptr<Value> address,
        std::unique_ptr<Expression> value
    )
        : address(std::move(address)),
          value(std::move(value)) {}

    StatementKind kind() const override {
        return StatementKind::Store;
    }
};