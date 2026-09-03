#pragma once

#include "IR.h"
#include <asmjit/asmjit.h>
#include <capstone/capstone.h>

#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <iostream>

// Turns an IR back into real x86-64 machine code using AsmJit,
// then re-disassembles the result for inspection/verification.
class Recompiler {
public:
    struct Result {
        std::string assembly;
        std::string hex;
    };

    // Emit machine code for every Assign/Store statement in the IR
    // and return its disassembly + raw hex bytes. Both compile to a
    // MOV; the only difference is whether the destination is a
    // register/flag (Assign) or memory (Store).
        Result compile(const IR& ir) {
            asmjit::CodeHolder code;
            code.init(asmjit::Environment::host());

            asmjit::x86::Assembler assembler(&code);

            for (const auto& stmt : ir.statements) {

                if (auto* assign = dynamic_cast<AssignStatement*>(stmt.get())) {
                    auto* dst = assign->dst.get();
                    auto* expr = assign->value.get();

                    if (!dst || !expr)
                        throw std::runtime_error("Invalid AssignStatement");

                    if (auto* binary = dynamic_cast<BinaryExpression*>(expr)) {
                        auto* left = dynamic_cast<Value*>(binary->left.get());
                        auto* right = dynamic_cast<Value*>(binary->right.get());

                        if (!left || !right)
                            throw std::runtime_error("Binary operands must be Value");

                        auto dst_operand = operand(dst);
                        auto left_operand = operand(left);
                        auto right_operand = operand(right);

                        asmjit::Error err;

                        switch (binary->operation()) {
                            case Operation::Add:
               
                                if (dst_operand == left_operand) {
                    
                                    err = assembler.emit(asmjit::x86::Inst::kIdAdd, dst_operand, right_operand);
                                } else {
            
                                    err = assembler.emit(asmjit::x86::Inst::kIdMov, dst_operand, left_operand);
                                    if (err == asmjit::kErrorOk) {
                                        err = assembler.emit(asmjit::x86::Inst::kIdAdd, dst_operand, right_operand);
                                    }
                                }
                                break;

                            case Operation::Sub:
                                if (dst_operand == left_operand) {
                                    err = assembler.emit(asmjit::x86::Inst::kIdSub, dst_operand, right_operand);
                                } else {
                                    err = assembler.emit(asmjit::x86::Inst::kIdMov, dst_operand, left_operand);
                                    if (err == asmjit::kErrorOk) {
                                        err = assembler.emit(asmjit::x86::Inst::kIdSub, dst_operand, right_operand);
                                    }
                                }
                                break;

                            default:
                                throw std::runtime_error("Unsupported binary operation");
                        }

                        if (err != asmjit::kErrorOk) {
                            throw std::runtime_error("AsmJit failed to emit instruction");
                        }

                    } else if (auto* src = dynamic_cast<Value*>(expr)) {
                        auto dst_operand = operand(dst);
                        auto src_operand = operand(src);

                        auto err = assembler.emit(
                            asmjit::x86::Inst::kIdMov,
                            dst_operand,
                            src_operand
                        );

                        if (err != asmjit::kErrorOk) {
                            throw std::runtime_error("AsmJit failed to emit MOV");
                        }
                    } else {
                        throw std::runtime_error("Unsupported expression type");
                    }

                } else if (auto* store = dynamic_cast<StoreStatement*>(stmt.get())) {
                    auto* address = dynamic_cast<Value*>(store->address.get());
                    auto* expr = store->value.get();

                    if (!address || !expr)
                        throw std::runtime_error("Invalid StoreStatement");

                    if (auto* binary = dynamic_cast<BinaryExpression*>(expr)) {
                        auto* left = dynamic_cast<Value*>(binary->left.get());
                        auto* right = dynamic_cast<Value*>(binary->right.get());

                        if (!left || !right)
                            throw std::runtime_error("Binary operands must be Value");

                        auto address_operand = operand(address);
                        auto left_operand = operand(left);
                        auto right_operand = operand(right);

                        asmjit::Error err;

                        switch (binary->operation()) {
                            case Operation::Add: {
                                auto temp = asmjit::x86::rax;
                                err = assembler.emit(asmjit::x86::Inst::kIdMov, temp, address_operand);
                                if (err == asmjit::kErrorOk) {
                                    err = assembler.emit(asmjit::x86::Inst::kIdAdd, temp, right_operand);
                                }
                                if (err == asmjit::kErrorOk) {
                                    err = assembler.emit(asmjit::x86::Inst::kIdMov, address_operand, temp);
                                }
                                break;
                            }

                            case Operation::Sub: {
                                auto temp = asmjit::x86::rax;
                                err = assembler.emit(asmjit::x86::Inst::kIdMov, temp, address_operand);
                                if (err == asmjit::kErrorOk) {
                                    err = assembler.emit(asmjit::x86::Inst::kIdSub, temp, right_operand);
                                }
                                if (err == asmjit::kErrorOk) {
                                    err = assembler.emit(asmjit::x86::Inst::kIdMov, address_operand, temp);
                                }
                                break;
                            }

                            default:
                                throw std::runtime_error("Unsupported binary operation for Store");
                        }

                        if (err != asmjit::kErrorOk) {
                            throw std::runtime_error("AsmJit failed to emit instruction");
                        }

                    } else if (auto* src = dynamic_cast<Value*>(expr)) {
                        auto dst_operand = operand(address);
                        auto src_operand = operand(src);

                        auto err = assembler.emit(
                            asmjit::x86::Inst::kIdMov,
                            dst_operand,
                            src_operand
                        );

                        if (err != asmjit::kErrorOk) {
                            throw std::runtime_error("AsmJit failed to emit MOV for Store");
                        }
                    } else {
                        throw std::runtime_error("Unsupported expression type for Store");
                    }

                } else {
                    continue;
                }
            }

            const auto* section = code.sectionById(0);

            const uint8_t* data = section->buffer().data();
            size_t size = section->buffer().size();

            return {
                disassemble(data, size),
                bytes_to_hex(data, size)
            };
        }

private:

    // Convert an IR Value (register/immediate/memory) into an
    // AsmJit operand. Throws on null or unsupported Value kinds.
    asmjit::Operand operand(Value* value) {

        if (!value)
            throw std::runtime_error("null Value");

        if (auto* reg = dynamic_cast<RegValue*>(value))
            return to_reg(reg->reg);

        if (auto* imm = dynamic_cast<ImmValue*>(value))
            return asmjit::Imm(imm->value);

        if (auto* mem = dynamic_cast<MemoryValue*>(value))
            return memory_operand(*mem);

        throw std::runtime_error("unsupported Value");
    }

    // Build an AsmJit memory operand from an IR MemoryValue.
    // NOTE: does not special-case a RIP base (rip-relative
    // addressing), which will fail in to_reg() below.
    asmjit::x86::Mem memory_operand(
        const MemoryValue& mem
    ) {
        auto* base =
            dynamic_cast<RegValue*>(mem.base.get());

        auto* index =
            dynamic_cast<RegValue*>(mem.index.get());

        auto base_reg =
            base
                ? to_reg(base->reg)
                : asmjit::x86::Gp{};

        auto index_reg =
            index
                ? to_reg(index->reg)
                : asmjit::x86::Gp{};

        return asmjit::x86::ptr(
            base_reg,
            index_reg,
            scale_to_shift(mem.scale),
            mem.displacement,
            mem.size
        );
    }

    // Map our Reg enum to an AsmJit 64-bit GP register.
    // Throws for anything without a direct GP mapping (e.g. RIP,
    // NONE, or flag registers).
    asmjit::x86::Gp to_reg(Reg reg) {
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

            default:
                throw std::runtime_error(
                    "invalid register"
                );
        }
    }

    // Convert a memory scale factor (1/2/4/8) into the shift
    // amount AsmJit expects for x86::ptr().
    uint32_t scale_to_shift(uint8_t scale) {
        switch (scale) {
            case 1: return 0;
            case 2: return 1;
            case 4: return 2;
            case 8: return 3;

            default:
                throw std::runtime_error(
                    "invalid memory scale"
                );
        }
    }

    // Disassemble raw machine code back to text, for display/debugging.
    std::string disassemble(
        const uint8_t* data,
        size_t size
    ) {
        csh handle;

        if (cs_open(
                CS_ARCH_X86,
                CS_MODE_64,
                &handle
            ) != CS_ERR_OK) {
            throw std::runtime_error(
                "capstone initialization failed"
            );
        }

        cs_insn* insn = nullptr;

        size_t count = cs_disasm(
            handle,
            data,
            size,
            0,
            0,
            &insn
        );

        std::stringstream ss;

        for (size_t i = 0; i < count; ++i) {
            ss << insn[i].mnemonic
               << " "
               << insn[i].op_str
               << "\n";
        }

        if (insn)
            cs_free(insn, count);

        cs_close(&handle);

        return ss.str();
    }

    // Format raw bytes as an uppercase, space-separated hex string.
    std::string bytes_to_hex(
        const uint8_t* data,
        size_t size
    ) {
        std::stringstream ss;

        for (size_t i = 0; i < size; ++i) {
            if (i)
                ss << ' ';

            ss << std::uppercase
               << std::hex
               << std::setw(2)
               << std::setfill('0')
               << static_cast<unsigned>(data[i]);
        }

        return ss.str();
    }
};