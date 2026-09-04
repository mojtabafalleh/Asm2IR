// Statement.h
#pragma once

#include "expression.h"
#include <memory>

enum class StatementKind {
    Assign,
    Store,
    ControlFlow
};

class Statement {
public:
    virtual ~Statement() = default;
    virtual StatementKind kind() const = 0;
};

class AssignStatement : public Statement {
public:
    uint64_t id;
    std::unique_ptr<Value> dst;
    std::unique_ptr<Expression> value;

    AssignStatement(
        uint64_t id,
        std::unique_ptr<Value> dst,
        std::unique_ptr<Expression> value
    )
        : id(id),
          dst(std::move(dst)),
          value(std::move(value)) {}

    StatementKind kind() const override {
        return StatementKind::Assign;
    }
};

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

enum class ControlFlowType {
    Jump,
    JumpConditional,
    Call,
    Return,
    ReturnFar,
    Int
};

class ControlFlowStatement : public Statement {
public:
    ControlFlowType type;
    explicit ControlFlowStatement(ControlFlowType type)
        : type(type) {}
    StatementKind kind() const override {
        return StatementKind::ControlFlow;
    }
    virtual bool is_unconditional() const {
        return type == ControlFlowType::Jump ||
               type == ControlFlowType::Call ||
               type == ControlFlowType::Return ||
               type == ControlFlowType::ReturnFar;
    }
    virtual bool is_conditional() const {
        return type == ControlFlowType::JumpConditional;
    }
    virtual bool is_call() const {
        return type == ControlFlowType::Call;
    }
    virtual bool is_return() const {
        return type == ControlFlowType::Return ||
               type == ControlFlowType::ReturnFar;
    }
};

class JumpStatement : public ControlFlowStatement {
public:
    uint64_t target;
    explicit JumpStatement(uint64_t target)
        : ControlFlowStatement(ControlFlowType::Jump),
          target(target) {}
};

// `condition` is a full expression (e.g. Eq(zf, 1)) rather than a bare
// Operation, so the recompiler has the actual operands to compare, not
// just the comparison kind. `target` is the taken-branch address; the
// not-taken case is implicit: it's simply the next statement in the IR,
// matching real x86 Jcc semantics.
class ConditionalJumpStatement : public ControlFlowStatement {
public:
    std::unique_ptr<Expression> condition;
    uint64_t target;
    ConditionalJumpStatement(std::unique_ptr<Expression> condition, uint64_t target)
        : ControlFlowStatement(ControlFlowType::JumpConditional),
          condition(std::move(condition)),
          target(target) {}
};

class CallStatement : public ControlFlowStatement {
public:
    uint64_t target;
    explicit CallStatement(uint64_t target)
        : ControlFlowStatement(ControlFlowType::Call),
          target(target) {}
};

class ReturnStatement : public ControlFlowStatement {
public:
    bool far;
    uint64_t immediate;
    explicit ReturnStatement(bool far = false, uint64_t imm = 0)
        : ControlFlowStatement(far ? ControlFlowType::ReturnFar : ControlFlowType::Return),
          far(far),
          immediate(imm) {}
    bool has_immediate() const {
        return immediate > 0;
    }
};

class InterruptStatement : public ControlFlowStatement {
public:
    uint8_t vector;
    explicit InterruptStatement(uint8_t vector)
        : ControlFlowStatement(ControlFlowType::Int),
          vector(vector) {}
};