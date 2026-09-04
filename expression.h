#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <vector>


enum class Reg : uint16_t {
    RAX, RBX, RCX, RDX,
    RSI, RDI, RBP, RSP,
    R8, R9, R10, R11,
    R12, R13, R14, R15,
    RIP,RFLAGS,

    CF, PF, AF, ZF, SF, OF,
    NONE
};


enum class ExpressionCategory {
    Value,
    Memory,
    Arithmetic,
    Bitwise,
    Logical,
    Compare,
    Other
};


enum class Operation {
    None,

    Load,

    Add,
    Sub,
    Rcr,
    Rcl,
    Mul,
    Div,
    Mod,
    Neg,

    Shl,
    Shr,
    Sar,
    Rol,
    Ror,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,

    LogicalAnd,
    LogicalOr,
    LogicalNot,

    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge
};


// Base node of every IR tree (values, unary ops, binary ops).
class Expression {
public:
    virtual ~Expression() = default;

    // Coarse classification, used for dispatch/printing.
    virtual ExpressionCategory category() const = 0;

    // Concrete operation this node represents (None for plain values).
    virtual Operation operation() const = 0;
};


// --------------------
// Values
// --------------------

// Base class for leaf nodes: registers, immediates, memory operands.
class Value : public Expression {
public:
    // All Value nodes report the same category.
    ExpressionCategory category() const override {
        return ExpressionCategory::Value;
    }

    // Values carry no operation of their own.
    Operation operation() const override {
        return Operation::None;
    }

    virtual std::unique_ptr<Value> clone() const = 0;
};


// A single machine/flag register reference.
//
// `reg` is always the canonical 64-bit identity (Reg::RAX, ...) ?
// there's no separate enumerator for eax/ax/al. `width` records
// which sub-register an instruction actually touched, in bits
// (8/16/32/64). This keeps Reg small while still letting the
// Lifter/Recompiler round-trip `mov eax, ...` vs `mov rax, ...`
// correctly.
class RegValue : public Value {
public:
    Reg reg;
    uint8_t width;

    explicit RegValue(Reg reg, uint8_t width = 64)
        : reg(reg), width(width) {}

    // Canonical 64-bit name, ignoring width. Used for addressing
    // (base/index inside a MemoryValue), where the access width of
    // the surrounding instruction doesn't affect the register name.
    std::string register_name() const {

        switch (reg) {

            case Reg::RAX: return "rax";
            case Reg::RBX: return "rbx";
            case Reg::RCX: return "rcx";
            case Reg::RDX: return "rdx";
            case Reg::RSI: return "rsi";
            case Reg::RDI: return "rdi";
            case Reg::RBP: return "rbp";
            case Reg::RSP: return "rsp";
            case Reg::R8:  return "r8";
            case Reg::R9:  return "r9";
            case Reg::R10: return "r10";
            case Reg::R11: return "r11";
            case Reg::R12: return "r12";
            case Reg::R13: return "r13";
            case Reg::R14: return "r14";
            case Reg::R15: return "r15";
            case Reg::RIP: return "rip";
            case Reg::RFLAGS: return "rflags";

            case Reg::CF: return "cf";
            case Reg::PF: return "pf";
            case Reg::AF: return "af";
            case Reg::ZF: return "zf";
            case Reg::SF: return "sf";
            case Reg::OF: return "of";

            default:
                throw std::runtime_error("invalid register");
        }
    }

    std::unique_ptr<Value> clone() const override {
        return std::make_unique<RegValue>(reg, width);
    }

    // Debug/IR text form: canonical name, plus an explicit
    // ":<width>" suffix whenever this access isn't the default
    // 64-bit one, e.g. "rax:32" for `mov eax, ...`. Flags and RIP
    // have no real sub-width, so they never get a suffix.
    std::string display_name() const {

        if (width == 64 || !has_sub_widths())
            return register_name();

        std::stringstream ss;
        ss << register_name() << ":" << static_cast<unsigned>(width);
        return ss.str();
    }

private:
    // True for general-purpose registers (real 8/16/32/64 sub-widths
    // exist); false for RIP/flags/NONE.
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


// A constant/immediate operand.
class ImmValue : public Value {
public:
    uint64_t value;

    explicit ImmValue(int64_t value)
        : value(static_cast<uint64_t>(value)) {}

    // Hex string representation, e.g. "0x10".
    std::string immediate_string() const {

        std::stringstream ss;

        ss << "0x"
           << std::uppercase
           << std::hex
           << value;

        return ss.str();
    }
    std::unique_ptr<Value> clone() const override {
        return std::make_unique<ImmValue>(static_cast<int64_t>(value));
    }
};


// A memory operand: [base + index*scale + disp], with an access size.
class MemoryValue : public Value {
public:
    uint8_t size;

    std::unique_ptr<Value> base;
    std::unique_ptr<Value> index;

    uint8_t scale;
    int64_t displacement;

    MemoryValue(
        uint8_t size,
        std::unique_ptr<Value> base = nullptr,
        std::unique_ptr<Value> index = nullptr,
        uint8_t scale = 1,
        int64_t displacement = 0
    )
        : size(size),
          base(std::move(base)),
          index(std::move(index)),
          scale(scale),
          displacement(displacement) {}

    // Memory operands are always classified as Memory.
    ExpressionCategory category() const override {
        return ExpressionCategory::Memory;
    }
    std::unique_ptr<Value> clone() const override {
        return std::make_unique<MemoryValue>(
            size,
            base ? base->clone() : nullptr,
            index ? index->clone() : nullptr,
            scale,
            displacement
        );
    }


    // Intel-style string, e.g. "qword ptr [rax + rbx*4 + 0x10]".
    std::string memory_string() const {

        auto* base_reg =
            dynamic_cast<RegValue*>(base.get());

        auto* index_reg =
            dynamic_cast<RegValue*>(index.get());

        std::stringstream ss;

        switch (size) {

            case 1:
                ss << "byte ptr ";
                break;

            case 2:
                ss << "word ptr ";
                break;

            case 4:
                ss << "dword ptr ";
                break;

            case 8:
                ss << "qword ptr ";
                break;

            default:
                throw std::runtime_error(
                    "unsupported memory size"
                );
        }

        ss << "[";

        bool has_term = false;

        if (base_reg) {

            ss << base_reg->register_name();

            has_term = true;
        }

        if (index_reg) {

            if (has_term)
                ss << " + ";

            ss << index_reg->register_name();

            if (scale != 1) {

                ss << " * "
                   << static_cast<unsigned>(scale);
            }

            has_term = true;
        }

        if (displacement != 0) {

            if (has_term) {

                if (displacement > 0) {

                    ss << " + 0x"
                       << std::uppercase
                       << std::hex
                       << static_cast<uint64_t>(
                            displacement
                       );

                } else {

                    ss << " - 0x"
                       << std::uppercase
                       << std::hex
                       << static_cast<uint64_t>(
                            -displacement
                       );
                }

            } else {

                ss << "0x"
                   << std::uppercase
                   << std::hex
                   << static_cast<uint64_t>(
                        displacement
                   );
            }
        }

        ss << "]";

        return ss.str();
    }
};


// --------------------
// Unary Expression
// --------------------

// A single-operand IR node (Load, Neg, BitNot, LogicalNot, ...).
class UnaryExpression : public Expression {
public:
    Operation op;
    std::unique_ptr<Expression> operand;

    UnaryExpression(
        Operation op,
        std::unique_ptr<Expression> operand
    )
        : op(op),
          operand(std::move(operand)) {}

    // The operation this node performs.
    Operation operation() const override {
        return op;
    }

    // Maps the operation to a coarse category.
    ExpressionCategory category() const override {

        switch (op) {

            case Operation::Load:
                return ExpressionCategory::Memory;

            case Operation::Neg:
                return ExpressionCategory::Arithmetic;

            case Operation::BitNot:
                return ExpressionCategory::Bitwise;

            case Operation::LogicalNot:
                return ExpressionCategory::Logical;

            default:
                return ExpressionCategory::Other;
        }
    }
};


// --------------------
// Binary Expression
// --------------------

// A two-operand, value-producing IR node (Add, Sub, Eq, ...).
// Side effects (Assign/Store) live in statement.h instead.
class BinaryExpression : public Expression {
public:
    Operation op;

    std::unique_ptr<Expression> left;
    std::unique_ptr<Expression> right;

    BinaryExpression(
        Operation op,
        std::unique_ptr<Expression> left,
        std::unique_ptr<Expression> right
    )
        : op(op),
          left(std::move(left)),
          right(std::move(right)) {}

    // The operation this node performs.
    Operation operation() const override {
        return op;
    }

    // Maps the operation to a coarse category.
    ExpressionCategory category() const override {

        switch (op) {

            case Operation::Add:
            case Operation::Sub:
            case Operation::Mul:
            case Operation::Div:
            case Operation::Mod:
                return ExpressionCategory::Arithmetic;

            case Operation::Shl:
            case Operation::Shr:
            case Operation::Sar:
            case Operation::Rol:
            case Operation::Ror:
            case Operation::BitAnd:
            case Operation::BitOr:
            case Operation::BitXor:
                return ExpressionCategory::Bitwise;

            case Operation::LogicalAnd:
            case Operation::LogicalOr:
                return ExpressionCategory::Logical;

            case Operation::Eq:
            case Operation::Ne:
            case Operation::Lt:
            case Operation::Le:
            case Operation::Gt:
            case Operation::Ge:
                return ExpressionCategory::Compare;

            default:
                return ExpressionCategory::Other;
        }
    }
};