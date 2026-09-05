// expression.h
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <vector>
#include <array>

enum class Reg : uint16_t {
    RAX, RBX, RCX, RDX,
    RSI, RDI, RBP, RSP,
    R8, R9, R10, R11,
    R12, R13, R14, R15,
    RIP, RFLAGS,
    CF, PF, AF, ZF, SF, OF,
    NONE
};

enum class ExpressionCategory {
    Value, Memory, Arithmetic, Bitwise, Logical, Compare, Conditional, Other
};

enum class Operation {
    None, Load, Add, Sub, Not, Rcr, Rcl, Mul, Div, Mod, Neg,
    Shl, Shr, Sar, Rol, Ror, BitAnd, BitOr, BitXor, BitNot,
    LogicalAnd, LogicalOr, LogicalNot, Eq, Ne, Lt, Le, Gt, Ge,
};

class Expression {
public:
    virtual ~Expression() = default;
    virtual ExpressionCategory category() const = 0;
    virtual Operation operation() const = 0;
};

class RegValue; // fwd decl for Value::as_reg()

class Value : public Expression {
public:
    int ssa_id = -1;
    ExpressionCategory category() const override { return ExpressionCategory::Value; }
    Operation operation() const override { return Operation::None; }
    virtual std::unique_ptr<Value> clone() const = 0;

    // Cheap, RTTI-free replacement for dynamic_cast<RegValue*>(value.get()).
    // Base Value is "not a register"; RegValue overrides it to return itself.
    virtual const RegValue* as_reg() const { return nullptr; }
};

class RegValue : public Value {
public:

    Reg reg;
    uint8_t width;

    explicit RegValue(Reg reg, uint8_t width = 64) : reg(reg), width(width) {}

    const RegValue* as_reg() const override { return this; }

    std::string register_name() const {
        switch (reg) {
            case Reg::RAX: return "rax"; case Reg::RBX: return "rbx";
            case Reg::RCX: return "rcx"; case Reg::RDX: return "rdx";
            case Reg::RSI: return "rsi"; case Reg::RDI: return "rdi";
            case Reg::RBP: return "rbp"; case Reg::RSP: return "rsp";
            case Reg::R8:  return "r8";  case Reg::R9:  return "r9";
            case Reg::R10: return "r10"; case Reg::R11: return "r11";
            case Reg::R12: return "r12"; case Reg::R13: return "r13";
            case Reg::R14: return "r14"; case Reg::R15: return "r15";
            case Reg::RIP: return "rip"; case Reg::RFLAGS: return "rflags";
            case Reg::CF: return "cf"; case Reg::PF: return "pf";
            case Reg::AF: return "af"; case Reg::ZF: return "zf";
            case Reg::SF: return "sf"; case Reg::OF: return "of";
            default: throw std::runtime_error("invalid register");
        }
    }

    std::unique_ptr<Value> clone() const override {
        auto v = std::make_unique<RegValue>(reg, width);
        v->ssa_id = ssa_id;
        return v;
    }

    std::string display_name() const {
        std::stringstream ss;
        ss << register_name();
        if (width != 64 && has_sub_widths())
            ss << ":" << static_cast<unsigned>(width);
        if (ssa_id >= 0)
            ss << "[" << ssa_id << "]";
        return ss.str();
    }

private:
    bool has_sub_widths() const {
        switch (reg) {
            case Reg::RAX: case Reg::RBX: case Reg::RCX: case Reg::RDX:
            case Reg::RSI: case Reg::RDI: case Reg::RBP: case Reg::RSP:
            case Reg::R8:  case Reg::R9:  case Reg::R10: case Reg::R11:
            case Reg::R12: case Reg::R13: case Reg::R14: case Reg::R15:
                return true;
            default:
                return false;
        }
    }
};

class RegisterFile {
public:
    RegisterFile() { versions.fill(-1); }

    void reset() { versions.fill(-1); }


    void tag_read(RegValue& reg) {
        int v = versions[idx(reg.reg)];
        reg.ssa_id = v < 0 ? 0 : v;
    }


    void tag_write(RegValue& reg) {
        reg.ssa_id = ++versions[idx(reg.reg)];
    }

private:
    static size_t idx(Reg r) { return static_cast<size_t>(r); }
    std::array<int, static_cast<size_t>(Reg::NONE)> versions;
};

class ImmValue : public Value {
public:
    uint64_t value;
    explicit ImmValue(int64_t value) : value(static_cast<uint64_t>(value)) {}

    std::string immediate_string() const {
        std::stringstream ss;
        ss << "0x" << std::uppercase << std::hex << value;
        return ss.str();
    }

    std::unique_ptr<Value> clone() const override {
        return std::make_unique<ImmValue>(static_cast<int64_t>(value));
    }
};

class MemoryValue : public Value {
public:
    uint8_t size;
    std::unique_ptr<Value> base;
    std::unique_ptr<Value> index;
    uint8_t scale;
    int64_t displacement;

    MemoryValue(uint8_t size, std::unique_ptr<Value> base = nullptr,
                std::unique_ptr<Value> index = nullptr, uint8_t scale = 1,
                int64_t displacement = 0)
        : size(size), base(std::move(base)), index(std::move(index)),
          scale(scale), displacement(displacement) {}

    ExpressionCategory category() const override { return ExpressionCategory::Memory; }

    std::unique_ptr<Value> clone() const override {
        return std::make_unique<MemoryValue>(
            size, base ? base->clone() : nullptr, index ? index->clone() : nullptr,
            scale, displacement);
    }

    std::string memory_string() const {
        // Was dynamic_cast<RegValue*>(...); base/index are always either
        // RegValue or null in this codebase, so the virtual as_reg() accessor
        // gives the same result without RTTI.
        const RegValue* base_reg = base ? base->as_reg() : nullptr;
        const RegValue* index_reg = index ? index->as_reg() : nullptr;

        std::stringstream ss;
        switch (size) {
            case 1: ss << "byte ptr "; break;
            case 2: ss << "word ptr "; break;
            case 4: ss << "dword ptr "; break;
            case 8: ss << "qword ptr "; break;
            default: throw std::runtime_error("unsupported memory size");
        }

        ss << "[";
        bool has_term = false;
        if (base_reg) {
            ss << base_reg->register_name();
            has_term = true;
        }
        if (index_reg) {
            if (has_term) ss << " + ";
            ss << index_reg->register_name();
            if (scale != 1) ss << " * " << static_cast<unsigned>(scale);
            has_term = true;
        }
        if (displacement != 0) {
            const uint64_t abs_disp = static_cast<uint64_t>(displacement > 0 ? displacement : -displacement);
            if (has_term) ss << (displacement > 0 ? " + 0x" : " - 0x");
            else ss << "0x";
            ss << std::uppercase << std::hex << abs_disp;
        }
        ss << "]";
        return ss.str();
    }
};

class UnaryExpression : public Expression {
public:
    Operation op;
    std::unique_ptr<Expression> operand;

    UnaryExpression(Operation op, std::unique_ptr<Expression> operand)
        : op(op), operand(std::move(operand)) {}

    Operation operation() const override { return op; }

    ExpressionCategory category() const override {
        switch (op) {
            case Operation::Load: return ExpressionCategory::Memory;
            case Operation::Neg: return ExpressionCategory::Arithmetic;
            case Operation::BitNot: return ExpressionCategory::Bitwise;
            case Operation::LogicalNot: return ExpressionCategory::Logical;
            default: return ExpressionCategory::Other;
        }
    }
};

class BinaryExpression : public Expression {
public:
    Operation op;
    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;

    BinaryExpression(Operation op, std::unique_ptr<Expression> left, std::unique_ptr<Expression> right)
        : op(op), left(std::move(left)), right(std::move(right)) {}

    Operation operation() const override { return op; }

    ExpressionCategory category() const override {
        switch (op) {
            case Operation::Add: case Operation::Sub:
            case Operation::Mul: case Operation::Div: case Operation::Mod:
                return ExpressionCategory::Arithmetic;
            case Operation::Shl: case Operation::Shr: case Operation::Sar:
            case Operation::Rol: case Operation::Ror:
            case Operation::BitAnd: case Operation::BitOr: case Operation::BitXor:
                return ExpressionCategory::Bitwise;
            case Operation::LogicalAnd: case Operation::LogicalOr:
                return ExpressionCategory::Logical;
            case Operation::Eq: case Operation::Ne: case Operation::Lt:
            case Operation::Le: case Operation::Gt: case Operation::Ge:
                return ExpressionCategory::Compare;
            default:
                return ExpressionCategory::Other;
        }
    }
};

class ConditionalExpression : public Expression {
public:
    std::unique_ptr<Expression> condition;
    std::unique_ptr<Expression> true_expr;
    std::unique_ptr<Expression> false_expr;

    ConditionalExpression(std::unique_ptr<Expression> condition,
                           std::unique_ptr<Expression> true_expr,
                           std::unique_ptr<Expression> false_expr)
        : condition(std::move(condition)), true_expr(std::move(true_expr)),
          false_expr(std::move(false_expr)) {}

    Operation operation() const override { return Operation::None; }
    ExpressionCategory category() const override { return ExpressionCategory::Conditional; }
};