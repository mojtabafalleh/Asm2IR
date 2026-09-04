// Lifter.h
#pragma once
#include <capstone/capstone.h>
#include "expression.h"
#include "Statement.h"
#include "IR.h"
#include <unordered_map>
#include <functional>
#include <memory>
#include <stdexcept>
#include <iostream>

class Lifter {
public:
    Lifter() {
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
            throw std::runtime_error("capstone initialization failed");
        cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
        register_handlers();
    }
    ~Lifter() { cs_close(&handle); }

    Reg reg(x86_reg r) {
        switch (r) {
            case X86_REG_RAX: case X86_REG_EAX: case X86_REG_AX: case X86_REG_AL: case X86_REG_AH: return Reg::RAX;
            case X86_REG_RBX: case X86_REG_EBX: case X86_REG_BX: case X86_REG_BL: case X86_REG_BH: return Reg::RBX;
            case X86_REG_RCX: case X86_REG_ECX: case X86_REG_CX: case X86_REG_CL: case X86_REG_CH: return Reg::RCX;
            case X86_REG_RDX: case X86_REG_EDX: case X86_REG_DX: case X86_REG_DL: case X86_REG_DH: return Reg::RDX;
            case X86_REG_RSI: case X86_REG_ESI: case X86_REG_SI: case X86_REG_SIL: return Reg::RSI;
            case X86_REG_RDI: case X86_REG_EDI: case X86_REG_DI: case X86_REG_DIL: return Reg::RDI;
            case X86_REG_RBP: case X86_REG_EBP: case X86_REG_BP: case X86_REG_BPL: return Reg::RBP;
            case X86_REG_RSP: case X86_REG_ESP: case X86_REG_SP: case X86_REG_SPL: return Reg::RSP;
            case X86_REG_R8:  case X86_REG_R8D:  case X86_REG_R8W:  case X86_REG_R8B:  return Reg::R8;
            case X86_REG_R9:  case X86_REG_R9D:  case X86_REG_R9W:  case X86_REG_R9B:  return Reg::R9;
            case X86_REG_R10: case X86_REG_R10D: case X86_REG_R10W: case X86_REG_R10B: return Reg::R10;
            case X86_REG_R11: case X86_REG_R11D: case X86_REG_R11W: case X86_REG_R11B: return Reg::R11;
            case X86_REG_R12: case X86_REG_R12D: case X86_REG_R12W: case X86_REG_R12B: return Reg::R12;
            case X86_REG_R13: case X86_REG_R13D: case X86_REG_R13W: case X86_REG_R13B: return Reg::R13;
            case X86_REG_R14: case X86_REG_R14D: case X86_REG_R14W: case X86_REG_R14B: return Reg::R14;
            case X86_REG_R15: case X86_REG_R15D: case X86_REG_R15W: case X86_REG_R15B: return Reg::R15;
            case X86_REG_RIP: return Reg::RIP;
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
                std::unique_ptr<Value> base = mem.base != X86_REG_INVALID
                    ? std::make_unique<RegValue>(reg(mem.base)) : nullptr;
                std::unique_ptr<Value> index = mem.index != X86_REG_INVALID
                    ? std::make_unique<RegValue>(reg(mem.index)) : nullptr;
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
        ir.add(std::make_unique<AssignStatement>(id, std::move(dst), std::move(value)));
    }

    void emit_assignment_or_store(std::unique_ptr<Value> dst, std::unique_ptr<Expression> src, IR& ir) {
        if (dst->category() == ExpressionCategory::Memory)
            ir.add(std::make_unique<StoreStatement>(std::move(dst), std::move(src)));
        else
            new_assign(std::move(dst), std::move(src), ir);
    }

    void emit_binary_operation(const cs_x86& x86, Operation op, IR& ir) {
        auto dst = operand(x86.operands[0]);
        auto src = operand(x86.operands[1]);
        auto expr = std::make_unique<BinaryExpression>(op, dst->clone(), std::move(src));
        emit_assignment_or_store(std::move(dst), std::move(expr), ir);
    }

    void emit_operation(const cs_x86& x86, Operation op, IR& ir) {
        auto src = operand(x86.operands[0]);
        auto dst = src->clone();
        new_assign(std::move(dst), std::make_unique<UnaryExpression>(op, std::move(src)), ir);
    }

    // --- stack helpers: push/pop/call/ret all boil down to "[rsp] <-> value" + adjust rsp ---

    std::unique_ptr<Value> stack_slot() {
        return std::make_unique<MemoryValue>(8, std::make_unique<RegValue>(Reg::RSP), nullptr, 1, 0);
    }

    void adjust_rsp(Operation op, int64_t amount, IR& ir) {
        auto expr = std::make_unique<BinaryExpression>(op, read_reg(Reg::RSP), std::make_unique<ImmValue>(amount));
        new_assign(std::make_unique<RegValue>(Reg::RSP), std::move(expr), ir);
    }

    void emit_stack_push(std::unique_ptr<Value> src, IR& ir) {
        adjust_rsp(Operation::Sub, 8, ir);
        emit_assignment_or_store(stack_slot(), std::move(src), ir);
    }

    void emit_stack_pop(std::unique_ptr<Value> dst, IR& ir) {
        emit_assignment_or_store(std::move(dst), stack_slot(), ir);
        adjust_rsp(Operation::Add, 8, ir);
    }

    void emit_call(const cs_x86& x86, IR& ir) {
        if (x86.op_count < 1) throw std::runtime_error("invalid call");
        emit_stack_push(std::make_unique<RegValue>(Reg::RIP), ir);
    }

    void emit_return(IR& ir) {
        emit_stack_pop(std::make_unique<RegValue>(Reg::RIP), ir);
    }

    // --- jumps ---

    std::unique_ptr<Expression> make_condition(x86_insn insn_id) {
        static const std::unordered_map<x86_insn, std::pair<Operation, int64_t>> table = {
            {X86_INS_JE,  {Operation::Eq, 1}},
            {X86_INS_JNE, {Operation::Ne, 1}},
            {X86_INS_JG,  {Operation::Gt, 0}},
            {X86_INS_JGE, {Operation::Ge, 0}},
            {X86_INS_JL,  {Operation::Lt, 0}},
            {X86_INS_JLE, {Operation::Le, 0}},
        };
        auto it = table.find(insn_id);
        if (it == table.end()) throw std::runtime_error("unsupported condition");
        return std::make_unique<BinaryExpression>(it->second.first, read_reg(Reg::ZF),
                                                    std::make_unique<ImmValue>(it->second.second));
    }

    // Unconditional jump: target is always known at lift time for direct
    // jumps, so this becomes a single first-class JumpStatement.
    void emit_jump(const cs_x86& x86, IR& ir) {
        if (x86.op_count < 1) throw std::runtime_error("invalid jump");
        ir.add(std::make_unique<JumpStatement>(static_cast<uint64_t>(x86.operands[0].imm)));
    }

    // Conditional jump: the "not taken" case needs no explicit target --
    // the IR is a flat, in-order statement stream, so falling through
    // simply means "go to the next statement", exactly like real x86.
    void emit_conditional_jump(const cs_x86& x86, IR& ir, x86_insn insn_id) {
        if (x86.op_count != 1 || x86.operands[0].type != X86_OP_IMM)
            throw std::runtime_error("invalid conditional jump");

        auto target = static_cast<int64_t>(x86.operands[0].imm);
        auto fallthrough = std::make_unique<BinaryExpression>(
            Operation::Add, read_reg(Reg::RIP), std::make_unique<ImmValue>(static_cast<int64_t>(x86.addr_size)));
        auto next_rip = std::make_unique<ConditionalExpression>(
            make_condition(insn_id), std::make_unique<ImmValue>(target), std::move(fallthrough));

        new_assign(std::make_unique<RegValue>(Reg::RIP), std::move(next_rip), ir);
    }

    IR lift(const uint8_t* code, size_t size) {
        cs_insn* insn;
        IR ir;
        size_t count = cs_disasm(handle, code, size, 0, 0, &insn);
        if (count == 0) throw std::runtime_error("disassembly failed");

        for (size_t i = 0; i < count; i++) {
            std::cout << insn[i].mnemonic << " " << insn[i].op_str << "\n";
            auto it = handlers.find(static_cast<x86_insn>(insn[i].id));
            if (it != handlers.end())
                it->second(insn[i].detail->x86, *this, ir);
        }
        cs_free(insn, count);
        return ir;
    }

private:
    using Handler = std::function<void(const cs_x86&, Lifter&, IR&)>;

    csh handle;
    std::unordered_map<x86_insn, Handler> handlers;
    void register_handlers();
};

// All instruction handling used to be one small class per opcode
// (MovHandler, PushHandler, BinaryOpHandler, ...), each just forwarding
// to a Lifter::emit_* method. That hierarchy added no behavior of its
// own, so it's collapsed into a single lambda table here.
inline void Lifter::register_handlers() {
    auto binop = [](Operation op) -> Handler {
        return [op](const cs_x86& x, Lifter& l, IR& ir) { l.emit_binary_operation(x, op, ir); };
    };
    auto unop = [](Operation op) -> Handler {
        return [op](const cs_x86& x, Lifter& l, IR& ir) { l.emit_operation(x, op, ir); };
    };
    auto condjump = [](x86_insn id) -> Handler {
        return [id](const cs_x86& x, Lifter& l, IR& ir) { l.emit_conditional_jump(x, ir, id); };
    };

    Handler mov = [](const cs_x86& x, Lifter& l, IR& ir) {
        l.emit_assignment_or_store(l.operand(x.operands[0]), l.operand(x.operands[1]), ir);
    };
    handlers[X86_INS_MOV] = mov;
    handlers[X86_INS_MOVABS] = mov;

    handlers[X86_INS_PUSH]   = [](const cs_x86& x, Lifter& l, IR& ir) { l.emit_stack_push(l.operand(x.operands[0]), ir); };
    handlers[X86_INS_POP]    = [](const cs_x86& x, Lifter& l, IR& ir) { l.emit_stack_pop(l.operand(x.operands[0]), ir); };
    handlers[X86_INS_PUSHFQ] = [](const cs_x86&, Lifter& l, IR& ir) { l.emit_stack_push(std::make_unique<RegValue>(Reg::RFLAGS), ir); };
    handlers[X86_INS_POPFQ]  = [](const cs_x86&, Lifter& l, IR& ir) { l.emit_stack_pop(std::make_unique<RegValue>(Reg::RFLAGS), ir); };

    handlers[X86_INS_LEA] = [](const cs_x86& x, Lifter& l, IR& ir) {
        if (x.op_count != 2) throw std::runtime_error("invalid LEA");
        if (x.operands[0].type != X86_OP_REG) throw std::runtime_error("LEA destination must be register");
        if (x.operands[1].type != X86_OP_MEM) throw std::runtime_error("LEA source must be memory operand");
        l.new_assign(l.operand(x.operands[0]), l.operand(x.operands[1]), ir);
    };

    handlers[X86_INS_JMP]  = [](const cs_x86& x, Lifter& l, IR& ir) { l.emit_jump(x, ir); };
    handlers[X86_INS_CALL] = [](const cs_x86& x, Lifter& l, IR& ir) { l.emit_call(x, ir); };
    handlers[X86_INS_RET]  = [](const cs_x86&, Lifter& l, IR& ir) { l.emit_return(ir); };

    handlers[X86_INS_ADD] = binop(Operation::Add);
    handlers[X86_INS_SUB] = binop(Operation::Sub);
    handlers[X86_INS_XOR] = binop(Operation::BitXor);
    handlers[X86_INS_SHR] = binop(Operation::Shr);
    handlers[X86_INS_RCR] = binop(Operation::Rcr);
    handlers[X86_INS_RCL] = binop(Operation::Rcl);
    handlers[X86_INS_NEG] = unop(Operation::Neg);

    for (x86_insn id : {X86_INS_JE, X86_INS_JNE, X86_INS_JG, X86_INS_JGE, X86_INS_JL, X86_INS_JLE})
        handlers[id] = condjump(id);
}