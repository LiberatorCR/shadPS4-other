// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <optional>
#include <span>
#include <vector>
#include "shader_recompiler/frontend/instruction.h"

namespace Shader::Gcn {
struct EntryWaveMinimum {
    u32 end_pc;
    u32 input_vgpr;
};

// Recognize the compiler's entry-EXEC reduction: restore non-helper entry lanes,
// fill every other lane with UINT_MAX, reduce each half, then min lanes31/63.
// Those physical inactive lanes do not exist in Vulkan. Only replace the final
// scalar minimum; preserve intermediate vector values and other lane reads.
inline std::vector<EntryWaveMinimum> FindEntryWaveMinima(std::span<const GcnInst> code) {
    std::vector<EntryWaveMinimum> matches;
    const auto write_width = [](const GcnInst& inst, const InstOperand& dst) -> u32 {
        switch (inst.opcode) {
        case Opcode::S_MOV_B32:
        case Opcode::S_LOAD_DWORD:
        case Opcode::S_BUFFER_LOAD_DWORD:
            return 1;
        case Opcode::S_MOV_B64:
        case Opcode::S_MEMTIME:
        case Opcode::S_LOAD_DWORDX2:
        case Opcode::S_BUFFER_LOAD_DWORDX2:
            return 2;
        case Opcode::S_LOAD_DWORDX4:
        case Opcode::S_BUFFER_LOAD_DWORDX4:
            return 4;
        case Opcode::S_LOAD_DWORDX8:
        case Opcode::S_BUFFER_LOAD_DWORDX8:
            return 8;
        case Opcode::S_LOAD_DWORDX16:
        case Opcode::S_BUFFER_LOAD_DWORDX16:
            return 16;
        default:
            switch (dst.type) {
            case ScalarType::Uint64:
            case ScalarType::Sint64:
            case ScalarType::Float64:
                return 2;
            case ScalarType::Uint16:
            case ScalarType::Sint16:
            case ScalarType::Float16:
            case ScalarType::Uint32:
            case ScalarType::Sint32:
            case ScalarType::Float32:
            case ScalarType::Bool:
                return 1;
            default:
                return 16; // Unknown writes remain conservative.
            }
        }
    };
    const auto plain = [](const InstOperand& operand) {
        return !operand.dpp && operand.sdwa_sel == SdwaSelector::Invalid &&
               !operand.input_modifier.neg && !operand.input_modifier.neg_hi &&
               !operand.input_modifier.abs && !operand.input_modifier.sext &&
               !operand.output_modifier.clamp && operand.output_modifier.multiplier == 0;
    };
    std::optional<u32> entry_exec;
    bool wqm = false;
    bool entry_unmodified = true;
    u32 pc = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        const auto& inst = code[i];
        const u32 start_pc = pc;
        pc += inst.length;
        if (!wqm && entry_unmodified && inst.opcode == Opcode::S_MOV_B64 &&
            inst.dst[0].field == OperandField::ScalarGPR &&
            inst.src[0].field == OperandField::ExecLo) {
            entry_exec = inst.dst[0].code;
            continue;
        }
        if (entry_exec && inst.opcode == Opcode::S_WQM_B64 &&
            inst.dst[0].field == OperandField::ExecLo &&
            inst.src[0].field == OperandField::ExecLo) {
            wqm = true;
            continue;
        }
        if (entry_exec && wqm && inst.opcode == Opcode::S_AND_SAVEEXEC_B64 &&
            inst.src[0].field == OperandField::ScalarGPR && inst.src[0].code == *entry_exec &&
            i + 21 < code.size()) {
            const auto& full = code[i + 1];
            const auto& fill = code[i + 2];
            bool valid = full.opcode == Opcode::S_ORN2_SAVEEXEC_B64 &&
                         full.dst[0].field == OperandField::VccLo &&
                         full.src[0].field == OperandField::ExecLo &&
                         fill.opcode == Opcode::V_CNDMASK_B32 &&
                         fill.encoding == InstEncoding::VOP2 && plain(fill.src[0]) &&
                         plain(fill.src[1]) && plain(fill.dst[0]) && fill.src[0].code == 193u &&
                         fill.src[0].field == OperandField::SignedConstIntNeg &&
                         fill.src[1].field == OperandField::VectorGPR &&
                         fill.dst[0].field == OperandField::VectorGPR;
            const u32 input = fill.src[1].code;
            const u32 accumulator = fill.dst[0].code;
            valid &= input != accumulator;
            for (u32 step = 0; step < 5 && valid; ++step) {
                const auto& shuffle = code[i + 3 + step * 3];
                const auto& wait = code[i + 4 + step * 3];
                const auto& minimum = code[i + 5 + step * 3];
                valid = shuffle.opcode == Opcode::DS_SWIZZLE_B32 &&
                        shuffle.control.ds.offset0 == 31u &&
                        shuffle.control.ds.offset1 == (16u >> step) * 4u &&
                        shuffle.src[0].field == OperandField::VectorGPR &&
                        shuffle.src[0].code == accumulator &&
                        shuffle.dst[0].field == OperandField::VectorGPR &&
                        shuffle.dst[0].code != input && shuffle.dst[0].code != accumulator &&
                        wait.opcode == Opcode::S_WAITCNT && minimum.opcode == Opcode::V_MIN_U32 &&
                        minimum.encoding == InstEncoding::VOP2 && plain(minimum.src[0]) &&
                        plain(minimum.src[1]) && plain(minimum.dst[0]) &&
                        minimum.dst[0].field == OperandField::VectorGPR &&
                        minimum.dst[0].code == accumulator &&
                        minimum.src[0].field == OperandField::VectorGPR &&
                        minimum.src[0].code == accumulator &&
                        minimum.src[1].field == OperandField::VectorGPR &&
                        minimum.src[1].code == shuffle.dst[0].code;
            }
            const auto& lo = code[i + 18];
            const auto& hi = code[i + 19];
            const auto& result = code[i + 20];
            valid &= lo.opcode == Opcode::V_READLANE_B32 && hi.opcode == Opcode::V_READLANE_B32 &&
                     lo.encoding == InstEncoding::VOP2 && hi.encoding == InstEncoding::VOP2 &&
                     lo.src[0].field == OperandField::VectorGPR && lo.src[0].code == accumulator &&
                     hi.src[0].field == OperandField::VectorGPR && hi.src[0].code == accumulator &&
                     lo.src[1].field == OperandField::SignedConstIntPos && lo.src[1].code == 159u &&
                     hi.src[1].field == OperandField::SignedConstIntPos && hi.src[1].code == 191u &&
                     lo.dst[0].field == OperandField::ScalarGPR &&
                     hi.dst[0].field == OperandField::ScalarGPR &&
                     result.opcode == Opcode::S_MIN_U32 &&
                     result.dst[0].field == OperandField::ScalarGPR &&
                     result.src[0].field == OperandField::ScalarGPR &&
                     result.src[0].code == lo.dst[0].code &&
                     result.src[1].field == OperandField::ScalarGPR &&
                     result.src[1].code == hi.dst[0].code;
            // S_MIN's comparison also writes SCC. It is safe to omit only if the
            // next instruction unconditionally overwrites it before any use.
            valid &= code[i + 21].opcode == Opcode::S_CMPK_EQ_U32;
            u32 end_pc = start_pc;
            for (size_t j = i; j <= i + 20; ++j)
                end_pc += code[j].length;
            // Reject alternate control-flow entrances into this sequence.
            u32 branch_pc = 0;
            for (const auto& branch : code) {
                if (branch.opcode == Opcode::S_SETPC_B64 || branch.opcode == Opcode::S_SWAPPC_B64 ||
                    branch.IsFork())
                    valid = false;
                if (branch.IsUnconditionalBranch() || branch.IsConditionalBranch()) {
                    const u32 target = branch.BranchTarget(branch_pc);
                    if (target > start_pc && target < end_pc)
                        valid = false;
                }
                branch_pc += branch.length;
            }
            if (valid)
                matches.push_back(EntryWaveMinimum{end_pc, input});
        }
        // Conservative lifetime proof for the saved entry mask. A scalar load
        // may span sixteen registers; unknown indirect writes invalidate it.
        if (entry_exec) {
            for (u32 d = 0; d < inst.dst_count; ++d) {
                const u32 last_written = inst.dst[d].code + write_width(inst, inst.dst[d]) - 1;
                if ((inst.dst[d].field == OperandField::ScalarGPR &&
                     inst.dst[d].code <= *entry_exec + 1 && last_written >= *entry_exec))
                    entry_exec.reset();
                if (!entry_exec)
                    break;
            }
            // EXEC changes do not overwrite the saved SGPR mask. Track its actual
            // lifetime across nested control flow, but reject indirect SGPR writes.
            if (inst.IsFork() || inst.opcode == Opcode::S_MOVRELD_B32 ||
                inst.opcode == Opcode::S_MOVRELD_B64)
                entry_exec.reset();
        }
        if (!wqm) {
            for (u32 d = 0; d < inst.dst_count; ++d) {
                if (inst.dst[d].field == OperandField::ExecLo)
                    entry_unmodified = false;
            }
            if (inst.IsCmpx() || inst.IsUnconditionalBranch() || inst.IsConditionalBranch() ||
                inst.IsFork() ||
                (inst.opcode >= Opcode::S_AND_SAVEEXEC_B64 &&
                 inst.opcode <= Opcode::S_XNOR_SAVEEXEC_B64))
                entry_unmodified = false;
        }
    }
    return matches;
}

inline std::optional<EntryWaveMinimum> FindEntryWaveMinimum(std::span<const GcnInst> code) {
    const auto matches = FindEntryWaveMinima(code);
    return matches.empty() ? std::nullopt : std::optional{matches.front()};
}
} // namespace Shader::Gcn
