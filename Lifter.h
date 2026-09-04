// Lifter.h
#pragma once

#include <capstone/capstone.h>
#include "expression.h"
#include "Statement.h"
#include "IR.h"
#include <unordered_map>
#include <memory>
#include <stdexcept>
#include <iostream>

class Lifter;
class InstructionHandler {
public:
    virtual ~InstructionHandler() = default;
    virtual void lift(const cs_x86& x86, Lifter& lifter, IR& ir) const = 0;
};

class Lifter {
public:
    Lifter() {
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
            throw std::runtime_error("capstone initialization failed");
        cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
        register_handlers();
    }

    ~Lifter() {
        cs_close(&handle);
    }

    Reg reg(x86_reg r) {
        switch (r) {
            case X86_REG_RAX: case X86_REG_EAX: case X86_REG_AX: case X86_REG_AL: case X86_REG_AH:
                return Reg::RAX;
            case X86_REG_RBX: case X86_REG_EBX: case X86_REG_BX: case X86_REG_BL: case X86_REG_BH:
                return Reg::RBX;
            case X86_REG_RCX: case X86_REG_ECX: case X86_REG_CX: case X86_REG_CL: case X86_REG_CH:
                return Reg::RCX;
            case X86_REG_RDX: case X86_REG_EDX: case X86_REG_DX: case X86_REG_DL: case X86_REG_DH:
                return Reg::RDX;
            case X86_REG_RSI: case X86_REG_ESI: case X86_REG_SI: case X86_REG_SIL:
                return Reg::RSI;
            case X86_REG_RDI: case X86_REG_EDI: case X86_REG_DI: case X86_REG_DIL:
                return Reg::RDI;
            case X86_REG_RBP: case X86_REG_EBP: case X86_REG_BP: case X86_REG_BPL:
                return Reg::RBP;
            case X86_REG_RSP: case X86_REG_ESP: case X86_REG_SP: case X86_REG_SPL:
                return Reg::RSP;
            case X86_REG_R8: case X86_REG_R8D: case X86_REG_R8W: case X86_REG_R8B:
                return Reg::R8;
            case X86_REG_R9: case X86_REG_R9D: case X86_REG_R9W: case X86_REG_R9B:
                return Reg::R9;
            case X86_REG_R10: case X86_REG_R10D: case X86_REG_R10W: case X86_REG_R10B:
                return Reg::R10;
            case X86_REG_R11: case X86_REG_R11D: case X86_REG_R11W: case X86_REG_R11B:
                return Reg::R11;
            case X86_REG_R12: case X86_REG_R12D: case X86_REG_R12W: case X86_REG_R12B:
                return Reg::R12;
            case X86_REG_R13: case X86_REG_R13D: case X86_REG_R13W: case X86_REG_R13B:
                return Reg::R13;
            case X86_REG_R14: case X86_REG_R14D: case X86_REG_R14W: case X86_REG_R14B:
                return Reg::R14;
            case X86_REG_R15: case X86_REG_R15D: case X86_REG_R15W: case X86_REG_R15B:
                return Reg::R15;
            case X86_REG_RIP:    return Reg::RIP;
            case X86_REG_EFLAGS: return Reg::RFLAGS;
            default: return Reg::NONE;
        }
    }

    std::unique_ptr<Value> operand(const cs_x86_op& op) {
        switch (op.type) {
        case X86_OP_REG:
            return std::make_unique<RegValue>(reg(op.reg), static_cast<uint8_t>(op.size * 8));
        case X86_OP_IMM:
            return std::make_unique<ImmValue>(op.imm);
        case X86_OP_MEM: {
            auto& mem = op.mem;
            std::unique_ptr<Value> base;
            std::unique_ptr<Value> index;
            if (mem.base != X86_REG_INVALID)
                base = std::make_unique<RegValue>(reg(mem.base));
            if (mem.index != X86_REG_INVALID)
                index = std::make_unique<RegValue>(reg(mem.index));
            return std::make_unique<MemoryValue>(op.size, std::move(base), std::move(index), mem.scale, mem.disp);
        }
        default:
            throw std::runtime_error("unsupported operand");
        }
    }

    std::unique_ptr<Value> read_reg(Reg r, uint8_t width = 64) {
        return std::make_unique<RegValue>(r, width);
    }

    void new_assign(std::unique_ptr<Value> dst, std::unique_ptr<Expression> value, IR& ir) {
        uint64_t id = ir.allocate_assign_id();
        auto assign = std::make_unique<AssignStatement>(id, std::move(dst), std::move(value));
        ir.add(std::move(assign));
    }

    void emit_assignment_or_store(std::unique_ptr<Value> dst, std::unique_ptr<Expression> src, IR& ir) {
        if (dst->category() == ExpressionCategory::Memory) {
            ir.add(std::make_unique<StoreStatement>(std::move(dst), std::move(src)));
        } else {
            new_assign(std::move(dst), std::move(src), ir);
        }
    }

    void emit_binary_operation(const cs_x86& x86, Operation op, IR& ir) {
        auto dst = operand(x86.operands[0]);
        auto src = operand(x86.operands[1]);
        auto left_val = dst->clone();
        auto expr = std::make_unique<BinaryExpression>(op, std::move(left_val), std::move(src));
        emit_assignment_or_store(std::move(dst), std::move(expr), ir);
    }

    void emit_stack_push(std::unique_ptr<Value> src, IR& ir) {
        auto rsp = std::make_unique<RegValue>(Reg::RSP);
        auto sub_expr = std::make_unique<BinaryExpression>(
            Operation::Sub, read_reg(Reg::RSP), std::make_unique<ImmValue>(8));
        new_assign(std::move(rsp), std::move(sub_expr), ir);

        auto mem_dst = std::make_unique<MemoryValue>(8, std::make_unique<RegValue>(Reg::RSP), nullptr, 1, 0);
        emit_assignment_or_store(std::move(mem_dst), std::move(src), ir);
    }

    void emit_stack_pop(std::unique_ptr<Value> dst, IR& ir) {
        auto mem_src = std::make_unique<MemoryValue>(8, std::make_unique<RegValue>(Reg::RSP), nullptr, 1, 0);
        emit_assignment_or_store(std::move(dst), std::move(mem_src), ir);

        auto rsp = std::make_unique<RegValue>(Reg::RSP);
        auto add_expr = std::make_unique<BinaryExpression>(
            Operation::Add, read_reg(Reg::RSP), std::make_unique<ImmValue>(8));
        new_assign(std::move(rsp), std::move(add_expr), ir);
    }

    std::unique_ptr<Expression> make_condition(x86_insn insn_id) {
        switch (insn_id) {
            case X86_INS_JE:
                return std::make_unique<BinaryExpression>(
                    Operation::Eq, read_reg(Reg::ZF), std::make_unique<ImmValue>(1));
            case X86_INS_JNE:
                return std::make_unique<BinaryExpression>(
                    Operation::Ne, read_reg(Reg::ZF), std::make_unique<ImmValue>(1));
            case X86_INS_JG:
                return std::make_unique<BinaryExpression>(
                    Operation::Gt, read_reg(Reg::ZF), std::make_unique<ImmValue>(0));
            case X86_INS_JGE:
                return std::make_unique<BinaryExpression>(
                    Operation::Ge, read_reg(Reg::ZF), std::make_unique<ImmValue>(0));
            case X86_INS_JL:
                return std::make_unique<BinaryExpression>(
                    Operation::Lt, read_reg(Reg::ZF), std::make_unique<ImmValue>(0));
            case X86_INS_JLE:
                return std::make_unique<BinaryExpression>(
                    Operation::Le, read_reg(Reg::ZF), std::make_unique<ImmValue>(0));
            default:
                throw std::runtime_error("unsupported condition");
        }
    }

    // Unconditional jump: target is always known at lift time for direct
    // jumps, so this becomes a single first-class JumpStatement.
    void emit_jump(const cs_x86& x86, IR& ir) {
        if (x86.op_count < 1) throw std::runtime_error("invalid jump");
        auto target = static_cast<uint64_t>(x86.operands[0].imm);
        ir.add(std::make_unique<JumpStatement>(target));
    }

    // Conditional jump: the "not taken" case needs no explicit target --
    // the IR is a flat, in-order statement stream, so falling through
    // simply means "go to the next statement", exactly like real x86.
    void emit_conditional_jump(
        const cs_x86& x86,
        IR& ir,
        x86_insn insn_id
    ) {

        
        if (x86.op_count != 1)
            throw std::runtime_error(
                "invalid conditional jump"
            );

        if (x86.operands[0].type != X86_OP_IMM)
            throw std::runtime_error(
                "conditional jump target must be immediate"
            );

        const uint64_t target =
            static_cast<uint64_t>(x86.operands[0].imm);


        auto condition =
            make_condition(insn_id);

        auto true_expr =
            std::make_unique<ImmValue>(
                static_cast<int64_t>(target)
            );

        auto false_expr =
            std::make_unique<BinaryExpression>(
                Operation::Add,
                read_reg(Reg::RIP),
                std::make_unique<ImmValue>(
                    static_cast<int64_t>(x86.addr_size)
                )
            );

        auto next_rip =
            std::make_unique<ConditionalExpression>(
                std::move(condition),
                std::move(true_expr),
                std::move(false_expr)
            );

        auto rip =
            std::make_unique<RegValue>(Reg::RIP);

        new_assign(
            std::move(rip),
            std::move(next_rip),
            ir
        );
    }

    // Direct call: only X86_OP_IMM targets are supported for now.
    // Indirect calls (register/memory targets) are a natural place to
    // extend this later.
    void emit_call(const cs_x86& x86, IR& ir) {
        if (x86.op_count < 1) throw std::runtime_error("invalid conditional jump");
        auto target = static_cast<uint64_t>(x86.operands[0].imm);
        auto rsp = std::make_unique<RegValue>(Reg::RSP);
        auto sub_expr = std::make_unique<BinaryExpression>(
            Operation::Sub, read_reg(Reg::RSP), std::make_unique<ImmValue>(8));
        new_assign(std::move(rsp), std::move(sub_expr), ir);

        auto mem_dst = std::make_unique<MemoryValue>(8, std::make_unique<RegValue>(Reg::RSP), nullptr, 1, 0);
        emit_assignment_or_store(std::move(mem_dst), std::make_unique<RegValue>(Reg::RIP), ir);
    }


    void emit_return(IR& ir) {
        auto mem_src = std::make_unique<MemoryValue>(8, std::make_unique<RegValue>(Reg::RSP), nullptr, 1, 0);
        emit_assignment_or_store(std::make_unique<RegValue>(Reg::RIP), std::move(mem_src), ir);

        auto rsp = std::make_unique<RegValue>(Reg::RSP);
        auto add_expr = std::make_unique<BinaryExpression>(
            Operation::Add, read_reg(Reg::RSP), std::make_unique<ImmValue>(8));
        new_assign(std::move(rsp), std::move(add_expr), ir);
    }

    IR lift(const uint8_t* code, size_t size) {
        cs_insn* insn;
        IR ir;

        size_t count = cs_disasm(handle, code, size, 0, 0, &insn);
        if (count == 0)
            throw std::runtime_error("disassembly failed");

        for (size_t i = 0; i < count; i++) {
            std::cout << insn[i].mnemonic << " " << insn[i].op_str << "\n";
            auto it = handlers.find(static_cast<x86_insn>(insn[i].id));
            if (it != handlers.end()) {
                it->second->lift(insn[i].detail->x86, *this, ir);
            }
        }

        cs_free(insn, count);
        return ir;
    }

private:
    csh handle;
    std::unordered_map<x86_insn, std::unique_ptr<InstructionHandler>> handlers;


    void register_handlers();
};

class BinaryOpHandler : public InstructionHandler {
    Operation op;
public:
    explicit BinaryOpHandler(Operation op) : op(op) {}
    void lift(const cs_x86& x86, Lifter& lifter, IR& ir) const override {
        lifter.emit_binary_operation(x86, op, ir);
    }
};

class MovHandler : public InstructionHandler {
public:
    void lift(const cs_x86& x86, Lifter& lifter, IR& ir) const override {
        auto dst = lifter.operand(x86.operands[0]);
        auto src = lifter.operand(x86.operands[1]);
        lifter.emit_assignment_or_store(std::move(dst), std::move(src), ir);
    }
};

class PushHandler : public InstructionHandler {
public:
    void lift(const cs_x86& x86, Lifter& lifter, IR& ir) const override {
        lifter.emit_stack_push(lifter.operand(x86.operands[0]), ir);
    }
};

class PopHandler : public InstructionHandler {
public:
    void lift(const cs_x86& x86, Lifter& lifter, IR& ir) const override {
        lifter.emit_stack_pop(lifter.operand(x86.operands[0]), ir);
    }
};

class PushfqHandler : public InstructionHandler {
public:
    void lift(const cs_x86&, Lifter& lifter, IR& ir) const override {
        lifter.emit_stack_push(std::make_unique<RegValue>(Reg::RFLAGS), ir);
    }
};

class LeaHandler : public InstructionHandler {
public:
    void lift(const cs_x86& x86, Lifter& lifter, IR& ir) const override {
        if (x86.op_count != 2)
            throw std::runtime_error("invalid LEA");

        if (x86.operands[0].type != X86_OP_REG)
            throw std::runtime_error("LEA destination must be register");

        if (x86.operands[1].type != X86_OP_MEM)
            throw std::runtime_error("LEA source must be memory operand");

        auto dst = lifter.operand(x86.operands[0]);
        auto src = lifter.operand(x86.operands[1]);

        lifter.new_assign(
            std::move(dst),
            std::move(src),
            ir
        );
    }
};

class PopfqHandler : public InstructionHandler {
public:
    void lift(const cs_x86&, Lifter& lifter, IR& ir) const override {
        lifter.emit_stack_pop(std::make_unique<RegValue>(Reg::RFLAGS), ir);
    }
};

class JumpHandler : public InstructionHandler {
public:
    void lift(const cs_x86& x86, Lifter& lifter, IR& ir) const override {
        lifter.emit_jump(x86, ir);
    }
};

class ConditionalJumpHandlerWithInsn : public InstructionHandler {
    x86_insn insn_id;
public:
    explicit ConditionalJumpHandlerWithInsn(x86_insn id) : insn_id(id) {}
    void lift(const cs_x86& x86, Lifter& lifter, IR& ir) const override {
        lifter.emit_conditional_jump(x86, ir,insn_id);
    }
};

class CallHandler : public InstructionHandler {
public:
    void lift(const cs_x86& x86, Lifter& lifter, IR& ir) const override {
        lifter.emit_call(x86, ir);
    }
};

class RetHandler : public InstructionHandler {
public:
    void lift(const cs_x86&, Lifter& lifter, IR& ir) const override {
        lifter.emit_return(ir);
    }
};

inline void Lifter::register_handlers() {
    handlers[X86_INS_MOV]    = std::make_unique<MovHandler>();
    handlers[X86_INS_MOVABS] = std::make_unique<MovHandler>();
    handlers[X86_INS_PUSH]   = std::make_unique<PushHandler>();
    handlers[X86_INS_POP]    = std::make_unique<PopHandler>();
    handlers[X86_INS_PUSHFQ] = std::make_unique<PushfqHandler>();
    handlers[X86_INS_LEA]    = std::make_unique<LeaHandler>();
    handlers[X86_INS_POPFQ]  = std::make_unique<PopfqHandler>();
    handlers[X86_INS_ADD]    = std::make_unique<BinaryOpHandler>(Operation::Add);
    handlers[X86_INS_SUB]    = std::make_unique<BinaryOpHandler>(Operation::Sub);
    handlers[X86_INS_XOR]    = std::make_unique<BinaryOpHandler>(Operation::BitXor);
    handlers[X86_INS_SHR]    = std::make_unique<BinaryOpHandler>(Operation::Shr);
    handlers[X86_INS_RCR]    = std::make_unique<BinaryOpHandler>(Operation::Rcr);
    handlers[X86_INS_RCL]    = std::make_unique<BinaryOpHandler>(Operation::Rcl);
    handlers[X86_INS_JMP]    = std::make_unique<JumpHandler>();
    handlers[X86_INS_JE]     = std::make_unique<ConditionalJumpHandlerWithInsn>(X86_INS_JE);
    handlers[X86_INS_JNE]    = std::make_unique<ConditionalJumpHandlerWithInsn>(X86_INS_JNE);
    handlers[X86_INS_JG]     = std::make_unique<ConditionalJumpHandlerWithInsn>(X86_INS_JG);
    handlers[X86_INS_JGE]    = std::make_unique<ConditionalJumpHandlerWithInsn>(X86_INS_JGE);
    handlers[X86_INS_JL]     = std::make_unique<ConditionalJumpHandlerWithInsn>(X86_INS_JL);
    handlers[X86_INS_JLE]    = std::make_unique<ConditionalJumpHandlerWithInsn>(X86_INS_JLE);
    handlers[X86_INS_CALL]   = std::make_unique<CallHandler>();
    handlers[X86_INS_RET]    = std::make_unique<RetHandler>();
}