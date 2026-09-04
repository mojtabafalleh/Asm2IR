#include <capstone/capstone.h>
#include "expression.h"
#include "statement.h"
#include "IR.h"
#include <iostream>

// Translates raw x86-64 machine code into this project's IR,
// using Capstone for disassembly/decoding.
class Lifter {
    csh handle;

    // Map a Capstone 64-bit GP register id to our Reg enum.
    // Falls back to Reg::NONE for anything not in this list
    // (sub-registers like eax/ax/al, segment/xmm regs, etc.).
    Reg reg(x86_reg r) {
        switch (r) {
            case X86_REG_RAX: return Reg::RAX;
            case X86_REG_RBX: return Reg::RBX;
            case X86_REG_RCX: return Reg::RCX;
            case X86_REG_RDX: return Reg::RDX;
            case X86_REG_RSI: return Reg::RSI;
            case X86_REG_RDI: return Reg::RDI;
            case X86_REG_RBP: return Reg::RBP;
            case X86_REG_RSP: return Reg::RSP;
            case X86_REG_R8:  return Reg::R8;
            case X86_REG_R9:  return Reg::R9;
            case X86_REG_R10: return Reg::R10;
            case X86_REG_R11: return Reg::R11;
            case X86_REG_R12: return Reg::R12;
            case X86_REG_R13: return Reg::R13;
            case X86_REG_R14: return Reg::R14;
            case X86_REG_R15: return Reg::R15;
            case X86_REG_RIP: return Reg::RIP;
            default: return Reg::NONE;
        }
    }

public:
    // Opens a Capstone x86-64 handle with instruction detail enabled.
    Lifter() {
        if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
            throw std::runtime_error("capstone initialization failed");

        cs_option(handle, CS_OPT_DETAIL, CS_OPT_ON);
    }

    // Releases the Capstone handle.
    ~Lifter() {
        cs_close(&handle);
    }

    // Convert one Capstone operand (register, immediate, or memory)
    // into an IR Value node. Throws on operand kinds not yet supported.
    std::unique_ptr<Value> operand(const cs_x86_op& op) {

        switch (op.type) {

        case X86_OP_REG:
            // op.size is Capstone's operand width in bytes (1/2/4/8
            // for al/ax/eax/rax, ...). We only keep a canonical
            // 64-bit Reg identity, so the actual access width has
            // to be carried separately on RegValue::width, or a
            // "mov eax, ..." would be indistinguishable from
            // "mov rax, ..." once lifted.
            return std::make_unique<RegValue>(
                reg(op.reg),
                static_cast<uint8_t>(op.size * 8)
            );

        case X86_OP_IMM:
            return std::make_unique<ImmValue>(
                op.imm
            );

        case X86_OP_MEM: {

            auto& mem = op.mem;

            std::unique_ptr<Value> base;
            std::unique_ptr<Value> index;

            if (mem.base != X86_REG_INVALID) {
                base = std::make_unique<RegValue>(
                    reg(mem.base)
                );
            }

            if (mem.index != X86_REG_INVALID) {
                index = std::make_unique<RegValue>(
                    reg(mem.index)
                );
            }

            return std::make_unique<MemoryValue>(
                op.size,
                std::move(base),
                std::move(index),
                mem.scale,
                mem.disp
            );
        }

        default:
            throw std::runtime_error(
                "unsupported operand"
            );
        }
    }



    // Disassemble a single instruction at `code` and lift it into
    // an IR. Only decodes one instruction (count == 1); throws on
    // decode failure or on an unhandled mnemonic.
        IR lift(const uint8_t* code, size_t size) {
            cs_insn* insn;
            IR ir;
    
     
            size_t count = cs_disasm(handle, code, size, 0x1000, 0, &insn);
    
            if (count == 0)
                throw std::runtime_error("disassembly failed");
    

            for (size_t i = 0; i < count; i++) {
                std::cout << insn[i].mnemonic << " " << insn[i].op_str << std::endl;
        
                auto& x86 = insn[i].detail->x86;
        
            switch (insn[i].id) {
                case X86_INS_MOV:
                case X86_INS_MOVABS: {
                    auto dst = operand(x86.operands[0]);
                    auto src = operand(x86.operands[1]);
                    emit_assignment_or_store(std::move(dst), std::move(src), ir);
                    break;
                }

                case X86_INS_PUSHFQ: {
                    // PUSH src
                    // 1. RSP = RSP - 8
                    // 2. [RSP] = rflag
                    
                    auto src = std::make_unique<RegValue>(Reg::RFLAGS);
                    
                    // RSP = RSP - 8
                    auto rsp = std::make_unique<RegValue>(Reg::RSP);
                    auto rsp_clone = rsp->clone();
                    auto eight = std::make_unique<ImmValue>(8);
                    
                    auto sub_expr = std::make_unique<BinaryExpression>(
                        Operation::Sub,
                        std::move(rsp_clone),
                        std::move(eight)
                    );
                    
                    emit_assignment_or_store(
                        std::move(rsp),
                        std::move(sub_expr),
                        ir
                    );
                    
                    // [RSP] = src
                    auto rsp_for_mem = std::make_unique<RegValue>(Reg::RSP);
                    auto mem_dst = std::make_unique<MemoryValue>(
                        8,  // size: 8 bytes for 64-bit push
                        std::move(rsp_for_mem),
                        nullptr,
                        1,
                        0
                    );
                    
                    emit_assignment_or_store(
                        std::move(mem_dst),
                        std::move(src),
                        ir
                    );
                    break;
                }
                case X86_INS_POPFQ: {
                    // PUSH src
                    // 1. RSP = RSP + 8
                    // 2. [RSP] = rflag
                    
                    auto src = std::make_unique<RegValue>(Reg::RFLAGS);
                    
                    // RSP = RSP + 8
                    auto rsp = std::make_unique<RegValue>(Reg::RSP);
                    auto rsp_clone = rsp->clone();
                    auto eight = std::make_unique<ImmValue>(8);
                                        // src =  [RSP] 
                    auto rsp_for_mem = std::make_unique<RegValue>(Reg::RSP);
                    auto mem_dst = std::make_unique<MemoryValue>(
                        8,  // size: 8 bytes for 64-bit push
                        std::move(rsp_for_mem),
                        nullptr,
                        1,
                        0
                    );
                    
                    emit_assignment_or_store(
                        std::move(src),
                        std::move(mem_dst),
                        ir
                    );
                    auto ADD_expr = std::make_unique<BinaryExpression>(
                        Operation::Add,
                        std::move(rsp_clone),
                        std::move(eight)
                    );
                    
                    emit_assignment_or_store(
                        std::move(rsp),
                        std::move(ADD_expr),
                        ir
                    );
                    

                    break;
                }
                case X86_INS_PUSH: {
                    // PUSH src
                    // 1. RSP = RSP - 8
                    // 2. [RSP] = src
                    
                    auto src = operand(x86.operands[0]);
                    
                    // RSP = RSP - 8
                    auto rsp = std::make_unique<RegValue>(Reg::RSP);
                    auto rsp_clone = rsp->clone();
                    auto eight = std::make_unique<ImmValue>(8);
                    
                    auto sub_expr = std::make_unique<BinaryExpression>(
                        Operation::Sub,
                        std::move(rsp_clone),
                        std::move(eight)
                    );
                    
                    emit_assignment_or_store(
                        std::move(rsp),
                        std::move(sub_expr),
                        ir
                    );
                    
                    // [RSP] = src
                    auto rsp_for_mem = std::make_unique<RegValue>(Reg::RSP);
                    auto mem_dst = std::make_unique<MemoryValue>(
                        8,  // size: 8 bytes for 64-bit push
                        std::move(rsp_for_mem),
                        nullptr,
                        1,
                        0
                    );
                    
                    emit_assignment_or_store(
                        std::move(mem_dst),
                        std::move(src),
                        ir
                    );
                    break;
                }

                case X86_INS_POP: {
                    // POP dst
                    // 1. dst = [RSP]
                    // 2. RSP = RSP + 8
                    
                    auto dst = operand(x86.operands[0]);
                    
                    // dst = [RSP]
                    auto rsp_for_mem = std::make_unique<RegValue>(Reg::RSP);
                    auto mem_src = std::make_unique<MemoryValue>(
                        8,  // size: 8 bytes for 64-bit pop
                        std::move(rsp_for_mem),
                        nullptr,
                        1,
                        0
                    );
                    
                    emit_assignment_or_store(
                        std::move(dst),
                        std::move(mem_src),
                        ir
                    );
                    
                    // RSP = RSP + 8
                    auto rsp = std::make_unique<RegValue>(Reg::RSP);
                    auto rsp_clone = rsp->clone();
                    auto eight = std::make_unique<ImmValue>(8);
                    
                    auto add_expr = std::make_unique<BinaryExpression>(
                        Operation::Add,
                        std::move(rsp_clone),
                        std::move(eight)
                    );
                    
                    emit_assignment_or_store(
                        std::move(rsp),
                        std::move(add_expr),
                        ir
                    );
                    break;
                }

                case X86_INS_ADD:
                    emit_binary_operation( x86, Operation::Add, ir);
                    break;

                case X86_INS_RCR:
                    emit_binary_operation( x86, Operation::Rcr, ir);
                    break;

                 case X86_INS_SHR:
                    emit_binary_operation( x86, Operation::Shr, ir);
                    break;
    
                case X86_INS_SUB:
                    emit_binary_operation( x86, Operation::Sub, ir);
                    break;
                case X86_INS_XOR:
                    emit_binary_operation( x86, Operation::BitXor, ir);
                    break;

            }
            }
    
            cs_free(insn, count);
            return ir;
        }




private:

    void emit_assignment_or_store(
        std::unique_ptr<Value> dst,
        std::unique_ptr<Expression> src,
        IR& ir
    ) {
        if (dst->category() == ExpressionCategory::Memory) {
            ir.add(std::make_unique<StoreStatement>(
                std::move(dst),
                std::move(src)
            ));
        } else {
            ir.add(std::make_unique<AssignStatement>(
                std::move(dst),
                std::move(src)
            ));
        }
    }

    void emit_binary_operation(
        const cs_x86& x86,
        Operation op,
        IR& ir
    ) {
        auto dst = operand(x86.operands[0]);
        auto src = operand(x86.operands[1]);
        
        auto dst_clone = dst->clone();
        
        auto expr = std::make_unique<BinaryExpression>(
            op,
            std::move(dst_clone),
            std::move(src)
        );
        
        emit_assignment_or_store(
            std::move(dst),
            std::move(expr),
            ir
        );
    }




};