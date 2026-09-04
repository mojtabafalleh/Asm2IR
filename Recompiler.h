#pragma once

#include "IR.h"
#include <asmjit/asmjit.h>
#include <capstone/capstone.h>

#include <sstream>
#include <iomanip>
#include <stdexcept>

class Recompiler {
public:
    struct Result {
        std::string assembly;
        std::string hex;
    };

    Result compile(const IR& ir) {
        asmjit::CodeHolder code;
        code.init(asmjit::Environment::host());

        asmjit::x86::Assembler assembler(&code);

        const auto& stmts = ir.statements;

        for (size_t i = 0; i < stmts.size(); ++i) {

            if (i + 1 < stmts.size() &&
                emit_push_pop(assembler, stmts[i].get(), stmts[i + 1].get())) {
                ++i;
                continue;
            }

            const auto& stmt = stmts[i];

            if (auto* assign = dynamic_cast<AssignStatement*>(stmt.get())) {
                if (!assign->dst || !assign->value)
                    throw std::runtime_error("Invalid AssignStatement");

                emit_write(assembler, operand(assign->dst.get()), assign->value.get(), false);

            } else if (auto* store = dynamic_cast<StoreStatement*>(stmt.get())) {
                if (!store->address || !store->value)
                    throw std::runtime_error("Invalid StoreStatement");

                emit_write(assembler, operand(store->address.get()), store->value.get(), true);
            }
        }

        const auto* section = code.sectionById(0);
        const uint8_t* data = section->buffer().data();
        size_t size = section->buffer().size();

        return { disassemble(data, size), bytes_to_hex(data, size) };
    }

private:

    // --------------------
    // Generic Assign/Store -> AsmJit emission
    // --------------------

    void check(asmjit::Error err, const char* what) {
        if (err != asmjit::kErrorOk)
            throw std::runtime_error(what);
    }

    asmjit::x86::Inst::Id to_inst_id(Operation op) {
        switch (op) {
            case Operation::Add:    return asmjit::x86::Inst::kIdAdd;
            case Operation::Sub:    return asmjit::x86::Inst::kIdSub;
            case Operation::BitXor: return asmjit::x86::Inst::kIdXor;
            case Operation::Shr:    return asmjit::x86::Inst::kIdShr;
            case Operation::Rcr:    return asmjit::x86::Inst::kIdRcr;
            case Operation::Rcl:    return asmjit::x86::Inst::kIdRcl;
            default: throw std::runtime_error("unsupported binary operation");
        }
    }

    // dst = left op right, where dst is a register/flag operand.
    void emit_binary_reg(
        asmjit::x86::Assembler& a, Operation op,
        asmjit::Operand dst, asmjit::Operand left, asmjit::Operand right
    ) {
        auto id = to_inst_id(op);

        if (dst == left) {
            check(a.emit(id, dst, right), "AsmJit failed to emit instruction");
        } else {
            check(a.emit(asmjit::x86::Inst::kIdMov, dst, left), "AsmJit failed to emit MOV");
            check(a.emit(id, dst, right), "AsmJit failed to emit instruction");
        }
    }

    // [address] = [address] op right, via a temp register (rax).
    void emit_binary_mem(
        asmjit::x86::Assembler& a, Operation op,
        asmjit::Operand address, asmjit::Operand right
    ) {
        auto id = to_inst_id(op);
        auto temp = asmjit::x86::rax;

        check(a.emit(asmjit::x86::Inst::kIdMov, temp, address), "AsmJit failed to emit MOV");
        check(a.emit(id, temp, right), "AsmJit failed to emit instruction");
        check(a.emit(asmjit::x86::Inst::kIdMov, address, temp), "AsmJit failed to emit MOV");
    }

    // Shared codegen for AssignStatement/StoreStatement: dst = expr.
    // `is_mem` picks the register vs memory binary-op strategy.
    void emit_write(asmjit::x86::Assembler& a, asmjit::Operand dst, Expression* expr, bool is_mem) {

        if (auto* binary = dynamic_cast<BinaryExpression*>(expr)) {
            auto* left = dynamic_cast<Value*>(binary->left.get());
            auto* right = dynamic_cast<Value*>(binary->right.get());

            if (!left || !right)
                throw std::runtime_error("Binary operands must be Value");

            auto right_operand = operand(right);

            if (is_mem)
                emit_binary_mem(a, binary->operation(), dst, right_operand);
            else
                emit_binary_reg(a, binary->operation(), dst, operand(left), right_operand);

        } else if (auto* src = dynamic_cast<Value*>(expr)) {
            check(a.emit(asmjit::x86::Inst::kIdMov, dst, operand(src)), "AsmJit failed to emit MOV");

        } else {
            throw std::runtime_error("Unsupported expression type");
        }
    }

    // --------------------
    // PUSH / POP / PUSHFQ / POPFQ fusion
    // --------------------
    //
    // The Lifter always expands push/pop-family instructions into
    // two statements (an RSP adjustment + a stack memory access).
    // Before generic codegen runs on `first`, check whether `first`
    // + `second` match one of those two-statement shapes and, if
    // so, emit the single real instruction instead.

    bool emit_push_pop(asmjit::x86::Assembler& assembler, Statement* first, Statement* second) {
        return try_emit_push(assembler, first, second) ||
               try_emit_pop(assembler, first, second);
    }

    bool try_emit_push(asmjit::x86::Assembler& assembler, Statement* first, Statement* second) {
        auto* assign = dynamic_cast<AssignStatement*>(first);
        auto* store = dynamic_cast<StoreStatement*>(second);

        if (!assign || !store)
            return false;

        if (!is_reg(assign->dst.get(), Reg::RSP))
            return false;

        if (!is_rsp_adjust(assign->value.get(), Operation::Sub, 8))
            return false;

        auto* address = dynamic_cast<Value*>(store->address.get());
        if (!address || !is_stack_top(address))
            return false;

        auto* src = dynamic_cast<Value*>(store->value.get());
        if (!src)
            return false;

        auto err = is_reg(src, Reg::RFLAGS)
            ? assembler.emit(asmjit::x86::Inst::kIdPushfq)
            : assembler.emit(asmjit::x86::Inst::kIdPush, operand(src));

        check(err, "AsmJit failed to emit PUSH");
        return true;
    }

    bool try_emit_pop(asmjit::x86::Assembler& assembler, Statement* first, Statement* second) {
        auto* assign1 = dynamic_cast<AssignStatement*>(first);
        auto* assign2 = dynamic_cast<AssignStatement*>(second);

        if (!assign1 || !assign2)
            return false;

        auto* value = dynamic_cast<Value*>(assign1->value.get());
        if (!value || !is_stack_top(value))
            return false;

        if (!is_reg(assign2->dst.get(), Reg::RSP))
            return false;

        if (!is_rsp_adjust(assign2->value.get(), Operation::Add, 8))
            return false;

        Value* dst = assign1->dst.get();
        if (!dst)
            return false;

        auto err = is_reg(dst, Reg::RFLAGS)
            ? assembler.emit(asmjit::x86::Inst::kIdPopfq)
            : assembler.emit(asmjit::x86::Inst::kIdPop, operand(dst));

        check(err, "AsmJit failed to emit POP");
        return true;
    }

    bool is_reg(Value* value, Reg reg) {
        auto* r = dynamic_cast<RegValue*>(value);
        return r && r->reg == reg;
    }

    // True if `expr` is `BinaryExpression(op, rsp, imm)` with imm == amount.
    bool is_rsp_adjust(Expression* expr, Operation op, uint64_t amount) {
        auto* binary = dynamic_cast<BinaryExpression*>(expr);
        if (!binary || binary->operation() != op)
            return false;

        auto* left = dynamic_cast<Value*>(binary->left.get());
        auto* right = dynamic_cast<Value*>(binary->right.get());
        if (!left || !right)
            return false;

        auto* imm = dynamic_cast<ImmValue*>(right);
        return is_reg(left, Reg::RSP) && imm && imm->value == amount;
    }

    // True for "[rsp]": 8-byte access, base == rsp, no index, no displacement.
    bool is_stack_top(Value* value) {
        auto* mem = dynamic_cast<MemoryValue*>(value);
        return mem &&
               mem->size == 8 &&
               mem->displacement == 0 &&
               !mem->index &&
               is_reg(mem->base.get(), Reg::RSP);
    }

    // --------------------
    // Value <-> AsmJit operand conversion
    // --------------------

    asmjit::Operand operand(Value* value) {
        if (!value)
            throw std::runtime_error("null Value");

        if (auto* reg = dynamic_cast<RegValue*>(value))
            return to_reg(reg->reg, reg->width);

        if (auto* imm = dynamic_cast<ImmValue*>(value))
            return asmjit::Imm(imm->value);

        if (auto* mem = dynamic_cast<MemoryValue*>(value))
            return memory_operand(*mem);

        throw std::runtime_error("unsupported Value");
    }

    // NOTE: does not special-case a RIP base (rip-relative addressing).
    asmjit::x86::Mem memory_operand(const MemoryValue& mem) {
        auto* base = dynamic_cast<RegValue*>(mem.base.get());
        auto* index = dynamic_cast<RegValue*>(mem.index.get());

        auto base_reg = base ? to_reg(base->reg, 64) : asmjit::x86::Gp{};
        auto index_reg = index ? to_reg(index->reg, 64) : asmjit::x86::Gp{};

        return asmjit::x86::ptr(base_reg, index_reg, scale_to_shift(mem.scale), mem.displacement, mem.size);
    }

    // (Reg::RAX, 32) -> eax, (Reg::RAX, 8) -> al, etc.
    asmjit::x86::Gp to_reg(Reg reg, uint8_t width = 64) {
        asmjit::x86::Gp reg64 = to_reg64(reg);

        switch (width) {
            case 8:  return reg64.r8Lo();
            case 16: return reg64.r16();
            case 32: return reg64.r32();
            case 64: return reg64;
            default: throw std::runtime_error("unsupported register width");
        }
    }

    asmjit::x86::Gp to_reg64(Reg reg) {
        switch (reg) {
            case Reg::RAX: return asmjit::x86::rax;
            case Reg::RBX: return asmjit::x86::rbx;
            case Reg::RCX: return asmjit::x86::rcx;
            case Reg::RDX: return asmjit::x86::rdx;
            case Reg::RSI: return asmjit::x86::rsi;
            case Reg::RDI: return asmjit::x86::rdi;
            case Reg::RBP: return asmjit::x86::rbp;
            case Reg::RSP: return asmjit::x86::rsp;
            case Reg::R8:  return asmjit::x86::r8;
            case Reg::R9:  return asmjit::x86::r9;
            case Reg::R10: return asmjit::x86::r10;
            case Reg::R11: return asmjit::x86::r11;
            case Reg::R12: return asmjit::x86::r12;
            case Reg::R13: return asmjit::x86::r13;
            case Reg::R14: return asmjit::x86::r14;
            case Reg::R15: return asmjit::x86::r15;
            default: throw std::runtime_error("invalid register");
        }
    }

    uint32_t scale_to_shift(uint8_t scale) {
        switch (scale) {
            case 1: return 0;
            case 2: return 1;
            case 4: return 2;
            case 8: return 3;
            default: throw std::runtime_error("invalid memory scale");
        }
    }

    // --------------------
    // Disassembly / formatting helpers
    // --------------------

    std::string disassemble(const uint8_t* data, size_t size) {
        csh handle;
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
            throw std::runtime_error("capstone initialization failed");

        cs_insn* insn = nullptr;
        size_t count = cs_disasm(handle, data, size, 0, 0, &insn);

        std::stringstream ss;
        for (size_t i = 0; i < count; ++i)
            ss << insn[i].mnemonic << " " << insn[i].op_str << "\n";

        if (insn)
            cs_free(insn, count);

        cs_close(&handle);
        return ss.str();
    }

    std::string bytes_to_hex(const uint8_t* data, size_t size) {
        std::stringstream ss;
        for (size_t i = 0; i < size; ++i) {
            if (i) ss << ' ';
            ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
               << static_cast<unsigned>(data[i]);
        }
        return ss.str();
    }
};