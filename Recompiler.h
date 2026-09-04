#pragma once

#include "IR.h"

#include <asmjit/asmjit.h>
#include <capstone/capstone.h>

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

class Recompiler {
public:
    struct Result {
        std::string assembly;
        std::string hex;
    };

    Result compile(const IR& ir) {
        asmjit::CodeHolder code;
        code.init(asmjit::Environment::host(), 0x00);
        asmjit::x86::Assembler assembler(&code);

        const auto& stmts = ir.statements;
        for (size_t i = 0; i < stmts.size(); ++i) {
            if (i + 1 < stmts.size() && emit_push_pop(assembler, stmts[i].get(), stmts[i + 1].get())) {
                ++i;
                continue;
            }
            emit_statement(ir, assembler, stmts[i].get());
        }

        // Resolve all labels after the complete block has been emitted.
        // asmjit calculates the correct relative displacement for JMP/Jcc.
        check(assembler.finalize(), "AsmJit failed to finalize code");

        auto* section = code.sectionById(0);
        if (!section)
            throw std::runtime_error("AsmJit code section not found");

        const uint8_t* data = section->buffer().data();
        const size_t size = section->buffer().size();

        return { disassemble(data, size), bytes_to_hex(data, size) };
    }

private:
    // NOTE: emit_jump/emit_call/emit_conditional_jump below hand asmjit
    // the raw target address directly, so no Label bookkeeping is
    // needed here. (A previous version kept an address->Label map and
    // an "invert address_starts" step for this; neither was ever
    // actually used, so both were removed.)

    void emit_statement(const IR& ir, asmjit::x86::Assembler& assembler, Statement* stmt) {
        if (!stmt)
            throw std::runtime_error("Null statement");

        if (auto* assign = dynamic_cast<AssignStatement*>(stmt)) {
            if (!assign->dst || !assign->value)
                throw std::runtime_error("Invalid AssignStatement");
            emit_write(assembler, operand(assign->dst.get()), assign->value.get(), false);
            return;
        }
        if (auto* store = dynamic_cast<StoreStatement*>(stmt)) {
            if (!store->address || !store->value)
                throw std::runtime_error("Invalid StoreStatement");
            emit_write(assembler, operand(store->address.get()), store->value.get(), true);
            return;
        }
        if (auto* jump = dynamic_cast<JumpStatement*>(stmt)) {
            emit_jump(assembler, jump->target);
            return;
        }
        if (auto* cjump = dynamic_cast<ConditionalJumpStatement*>(stmt)) {
            emit_conditional_jump(assembler, cjump);
            return;
        }
        if (auto* call = dynamic_cast<CallStatement*>(stmt)) {
            emit_call(assembler, call->target);
            return;
        }
        if (dynamic_cast<ReturnStatement*>(stmt)) {
            check(assembler.ret(), "AsmJit failed to emit RET");
            return;
        }

        throw std::runtime_error("Unsupported statement type");
    }

    void check(asmjit::Error err, const char* what) {
        if (err != asmjit::kErrorOk)
            throw std::runtime_error(what);
    }

    /* ---------------------------- CONTROL FLOW ---------------------------- */

    void emit_jump(asmjit::x86::Assembler& assembler, uint64_t target) {
        check(assembler.jmp(target), "AsmJit failed to emit JMP");
    }

    void emit_call(asmjit::x86::Assembler& assembler, uint64_t target) {
        check(assembler.call(target), "AsmJit failed to emit CALL");
    }

    /*
     * ConditionalJumpStatement::condition is a BinaryExpression.
     *
     * There are two shapes it can take:
     *
     *   1) A real comparison between two values, e.g. Eq(rax, 5):
     *          CMP left, right
     *          Jcc target
     *
     *   2) A flag test, e.g. Eq(zf, 1) -- this is what the Lifter
     *      produces for conditional jumps. `zf` here does NOT mean
     *      "the register named zf"; it means "the zero flag, as left
     *      by whatever instruction ran right before this jump". Flags
     *      aren't real, readable GP registers, so there is nothing to
     *      CMP -- the flag is already live, and the condition lowers
     *      straight to the matching Jcc.
     *
     * The not-taken path needs no explicit jump either way: the caller
     * (compile()) simply continues on to the next statement.
     */
    void emit_conditional_jump(asmjit::x86::Assembler& assembler, ConditionalJumpStatement* cjump) {
        auto* condition = dynamic_cast<BinaryExpression*>(cjump->condition.get());
        if (!condition)
            throw std::runtime_error("ConditionalJumpStatement requires a BinaryExpression condition");

        auto* left = dynamic_cast<Value*>(condition->left.get());
        auto* right = dynamic_cast<Value*>(condition->right.get());
        if (!left || !right)
            throw std::runtime_error("Condition operands must be Value");

        check(assembler.emit(asmjit::x86::Inst::kIdCmp, operand(left), operand(right)),
              "AsmJit failed to emit CMP");
        check(assembler.j(to_cond_code(condition->operation()), cjump->target),
              "AsmJit failed to emit conditional JMP");
    }

    asmjit::x86::CondCode to_cond_code(Operation op) {
        switch (op) {
            case Operation::Eq: return asmjit::x86::CondCode::kE;
            case Operation::Ne: return asmjit::x86::CondCode::kNE;
            case Operation::Lt: return asmjit::x86::CondCode::kL;
            case Operation::Le: return asmjit::x86::CondCode::kLE;
            case Operation::Gt: return asmjit::x86::CondCode::kG;
            case Operation::Ge: return asmjit::x86::CondCode::kGE;
            default: throw std::runtime_error("Unsupported conditional operation");
        }
    }

    /* --------------------- NORMAL REGISTER / MEMORY WRITES --------------------- */

    void emit_binary_reg(asmjit::x86::Assembler& assembler, Operation op,
                          asmjit::Operand dst, asmjit::Operand left, asmjit::Operand right) {
        auto id = to_inst_id(op);
        if (dst != left)
            check(assembler.emit(asmjit::x86::Inst::kIdMov, dst, left), "AsmJit failed to emit MOV");
        check(assembler.emit(id, dst, right), "AsmJit failed to emit instruction");
    }

    void emit_binary_mem(asmjit::x86::Assembler& assembler, Operation op,
                          asmjit::Operand address, asmjit::Operand right) {
        auto id = to_inst_id(op);
        auto temp = asmjit::x86::rax;
        check(assembler.emit(asmjit::x86::Inst::kIdMov, temp, address), "AsmJit failed to emit MOV");
        check(assembler.emit(id, temp, right), "AsmJit failed to emit instruction");
        check(assembler.emit(asmjit::x86::Inst::kIdMov, address, temp), "AsmJit failed to emit MOV");
    }

    void emit_unary(asmjit::x86::Assembler& assembler, asmjit::x86::Inst::Id id,
                     asmjit::Operand dst, asmjit::Operand src) {
        if (dst != src)
            check(assembler.emit(asmjit::x86::Inst::kIdMov, dst, src), "AsmJit failed to emit MOV");
        check(assembler.emit(id, dst), "AsmJit failed to emit unary instruction");
    }

    void emit_write(asmjit::x86::Assembler& assembler, asmjit::Operand dst, Expression* expr, bool is_mem) {
        if (auto* unary = dynamic_cast<UnaryExpression*>(expr)) {
            auto* value = dynamic_cast<Value*>(unary->operand.get());
            if (!value)
                throw std::runtime_error("Unary operand must be Value");

            if (unary->operation() == Operation::LogicalNot) {
                emit_unary(assembler, asmjit::x86::Inst::kIdNot, dst, operand(value));
                return;
            }
            if (unary->operation() == Operation::Neg) {
                emit_unary(assembler, asmjit::x86::Inst::kIdNeg, dst, operand(value));
                return;
            }
            throw std::runtime_error("Unsupported unary operation");
        }

        if (auto* binary = dynamic_cast<BinaryExpression*>(expr)) {
            auto* left = dynamic_cast<Value*>(binary->left.get());
            auto* right = dynamic_cast<Value*>(binary->right.get());
            if (!left || !right)
                throw std::runtime_error("Binary operands must be Value");

            auto right_operand = operand(right);
            if (is_mem)
                emit_binary_mem(assembler, binary->operation(), dst, right_operand);
            else
                emit_binary_reg(assembler, binary->operation(), dst, operand(left), right_operand);
            return;
        }

        if (auto* src = dynamic_cast<Value*>(expr)) {
            check(assembler.emit(asmjit::x86::Inst::kIdMov, dst, operand(src)), "AsmJit failed to emit MOV");
            return;
        }

        throw std::runtime_error("Unsupported expression type");
    }

    /* ------------------------------- PUSH / POP ------------------------------- */

    bool emit_push_pop(asmjit::x86::Assembler& assembler, Statement* first, Statement* second) {
        return try_emit_push(assembler, first, second) || try_emit_pop(assembler, first, second);
    }

    bool try_emit_push(asmjit::x86::Assembler& assembler, Statement* first, Statement* second) {
        auto* assign = dynamic_cast<AssignStatement*>(first);
        auto* store = dynamic_cast<StoreStatement*>(second);
        if (!assign || !store)
            return false;
        if (!is_reg(assign->dst.get(), Reg::RSP) || !is_rsp_adjust(assign->value.get(), Operation::Sub, 8))
            return false;

        auto* address = dynamic_cast<Value*>(store->address.get());
        if (!address || !is_stack_top(address))
            return false;

        auto* src = dynamic_cast<Value*>(store->value.get());
        if (!src)
            return false;

        asmjit::Error err = is_reg(src, Reg::RFLAGS)
            ? assembler.pushfq()
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
        if (!is_reg(assign2->dst.get(), Reg::RSP) || !is_rsp_adjust(assign2->value.get(), Operation::Add, 8))
            return false;

        Value* dst = assign1->dst.get();
        if (!dst)
            return false;

        asmjit::Error err = is_reg(dst, Reg::RFLAGS)
            ? assembler.popfq()
            : assembler.emit(asmjit::x86::Inst::kIdPop, operand(dst));
        check(err, "AsmJit failed to emit POP");
        return true;
    }

    bool is_reg(Value* value, Reg reg) {
        auto* r = dynamic_cast<RegValue*>(value);
        return r && r->reg == reg;
    }

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

    bool is_stack_top(Value* value) {
        auto* mem = dynamic_cast<MemoryValue*>(value);
        return mem && mem->size == 8 && mem->displacement == 0 && !mem->index && is_reg(mem->base.get(), Reg::RSP);
    }

    /* --------------------------------- OPERANDS -------------------------------- */

    asmjit::Operand operand(Value* value) {
        if (!value)
            throw std::runtime_error("null Value");

        if (auto* reg = dynamic_cast<RegValue*>(value)) {
            // RIP must never arrive here.
            if (reg->reg == Reg::RIP)
                throw std::runtime_error("RIP cannot be used as a writable general-purpose operand");
            return to_reg(reg->reg, reg->width);
        }
        if (auto* imm = dynamic_cast<ImmValue*>(value))
            return asmjit::Imm(imm->value);
        if (auto* mem = dynamic_cast<MemoryValue*>(value))
            return memory_operand(*mem);

        throw std::runtime_error("Unsupported Value");
    }

    asmjit::x86::Mem memory_operand(const MemoryValue& mem) {
        auto* base = dynamic_cast<RegValue*>(mem.base.get());
        auto* index = dynamic_cast<RegValue*>(mem.index.get());
        auto base_reg = base ? to_reg(base->reg, 64) : asmjit::x86::Gp{};
        auto index_reg = index ? to_reg(index->reg, 64) : asmjit::x86::Gp{};
        return asmjit::x86::ptr(base_reg, index_reg, scale_to_shift(mem.scale), mem.displacement, mem.size);
    }

    asmjit::x86::Gp to_reg(Reg reg, uint8_t width = 64) {
        asmjit::x86::Gp reg64 = to_reg64(reg);
        switch (width) {
            case 8:  return reg64.r8Lo();
            case 16: return reg64.r16();
            case 32: return reg64.r32();
            case 64: return reg64;
            default: throw std::runtime_error("Unsupported register width");
        }
    }

    // Reg::RAX..Reg::R15 are declared in that exact order in expression.h,
    // matching the x86 GP register numbering -- so a plain lookup table
    // replaces what used to be a 16-case switch statement.
    asmjit::x86::Gp to_reg64(Reg reg) {
        static const asmjit::x86::Gp regs[] = {
            asmjit::x86::rax, asmjit::x86::rbx, asmjit::x86::rcx, asmjit::x86::rdx,
            asmjit::x86::rsi, asmjit::x86::rdi, asmjit::x86::rbp, asmjit::x86::rsp,
            asmjit::x86::r8,  asmjit::x86::r9,  asmjit::x86::r10, asmjit::x86::r11,
            asmjit::x86::r12, asmjit::x86::r13, asmjit::x86::r14, asmjit::x86::r15,
        };
        auto idx = static_cast<size_t>(reg);
        if (idx >= sizeof(regs) / sizeof(regs[0]))
            throw std::runtime_error("Invalid general-purpose register"); // e.g. RIP, deliberately excluded
        return regs[idx];
    }

    uint32_t scale_to_shift(uint8_t scale) {
        switch (scale) {
            case 1: return 0;
            case 2: return 1;
            case 4: return 2;
            case 8: return 3;
            default: throw std::runtime_error("Invalid memory scale");
        }
    }

    /* ----------------------------- NORMAL OPERATIONS ---------------------------- */

    asmjit::x86::Inst::Id to_inst_id(Operation op) {
        switch (op) {
            case Operation::Add:    return asmjit::x86::Inst::kIdAdd;
            case Operation::Sub:    return asmjit::x86::Inst::kIdSub;
            case Operation::BitXor: return asmjit::x86::Inst::kIdXor;
            case Operation::Shr:    return asmjit::x86::Inst::kIdShr;
            case Operation::Rcr:    return asmjit::x86::Inst::kIdRcr;
            case Operation::Rcl:    return asmjit::x86::Inst::kIdRcl;
            default: throw std::runtime_error("Unsupported binary operation");
        }
    }

    /* ------------------------------- DISASSEMBLY -------------------------------- */

    std::string disassemble(const uint8_t* data, size_t size) {
        csh handle;
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
            throw std::runtime_error("Capstone initialization failed");

        cs_insn* insn = nullptr;
        size_t count = cs_disasm(handle, data, size, 0, 0, &insn);

        std::stringstream ss;
        for (size_t i = 0; i < count; ++i)
            ss << insn[i].mnemonic << " " << insn[i].op_str << "\n";

        if (insn) cs_free(insn, count);
        cs_close(&handle);
        return ss.str();
    }

    std::string bytes_to_hex(const uint8_t* data, size_t size) {
        std::stringstream ss;
        for (size_t i = 0; i < size; ++i) {
            if (i) ss << ' ';
            ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(data[i]);
        }
        return ss.str();
    }
};